/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BlurFilter.h"
#include <SkCanvas.h>
#include <SkData.h>
#include <SkPaint.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <log/log.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

BlurFilter::BlurFilter() {
    SkString blurString(R"(
        in shader input;
        uniform float in_inverseScale;
        uniform float2 in_blurOffset;

        half4 main(float2 xy) {
            float2 scaled_xy = float2(xy.x * in_inverseScale, xy.y * in_inverseScale);

            float4 c = float4(sample(input, scaled_xy));
            c += float4(sample(input, scaled_xy + float2( in_blurOffset.x,  in_blurOffset.y)));
            c += float4(sample(input, scaled_xy + float2( in_blurOffset.x, -in_blurOffset.y)));
            c += float4(sample(input, scaled_xy + float2(-in_blurOffset.x,  in_blurOffset.y)));
            c += float4(sample(input, scaled_xy + float2(-in_blurOffset.x, -in_blurOffset.y)));

            return half4(c.rgb * 0.2, 1.0);
        }
    )");

    auto [blurEffect, error] = SkRuntimeEffect::Make(blurString);
    if (!blurEffect) {
        LOG_ALWAYS_FATAL("RuntimeShader error: %s", error.c_str());
    }
    mBlurEffect = std::move(blurEffect);
}

void BlurFilter::draw(SkCanvas* canvas, sk_sp<SkSurface> input, const uint32_t blurRadius) const {
    ATRACE_CALL();
    // Kawase is an approximation of Gaussian, but it behaves differently from it.
    // A radius transformation is required for approximating them, and also to introduce
    // non-integer steps, necessary to smoothly interpolate large radii.
    float tmpRadius = (float)blurRadius / 6.0f;
    float numberOfPasses = std::min(kMaxPasses, (uint32_t)ceil(tmpRadius));
    float radiusByPasses = tmpRadius / (float)numberOfPasses;

    SkImageInfo scaledInfo = SkImageInfo::MakeN32Premul((float)input->width() * kInputScale,
                                                        (float)input->height() * kInputScale);
    auto drawSurface = canvas->makeSurface(scaledInfo);

    const float stepX = radiusByPasses;
    const float stepY = radiusByPasses;

    // start by drawing and downscaling and doing the first blur pass
    SkFilterOptions linear = {SkSamplingMode::kLinear, SkMipmapMode::kNone};
    SkRuntimeShaderBuilder blurBuilder(mBlurEffect);
    blurBuilder.child("input") =
            input->makeImageSnapshot()->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
    blurBuilder.uniform("in_inverseScale") = kInverseInputScale;
    blurBuilder.uniform("in_blurOffset") =
            SkV2{stepX * kInverseInputScale, stepY * kInverseInputScale};

    {
        // limit the lifetime of the input surface's snapshot to ensure that it goes out of
        // scope before the surface is written into to avoid any copy-on-write behavior.
        SkPaint paint;
        paint.setShader(blurBuilder.makeShader(nullptr, false));
        paint.setFilterQuality(kLow_SkFilterQuality);
        drawSurface->getCanvas()->drawIRect(scaledInfo.bounds(), paint);
        blurBuilder.child("input") = nullptr;
    }

    // And now we'll ping pong between our surfaces, to accumulate the result of various offsets.
    auto lastDrawTarget = drawSurface;
    if (numberOfPasses > 1) {
        auto readSurface = drawSurface;
        drawSurface = canvas->makeSurface(scaledInfo);

        for (auto i = 1; i < numberOfPasses; i++) {
            const float stepScale = (float)i * kInputScale;

            blurBuilder.child("input") =
                    readSurface->makeImageSnapshot()->makeShader(SkTileMode::kClamp,
                                                                 SkTileMode::kClamp, linear);
            blurBuilder.uniform("in_inverseScale") = 1.0f;
            blurBuilder.uniform("in_blurOffset") = SkV2{stepX * stepScale, stepY * stepScale};

            SkPaint paint;
            paint.setShader(blurBuilder.makeShader(nullptr, false));
            paint.setFilterQuality(kLow_SkFilterQuality);
            drawSurface->getCanvas()->drawIRect(scaledInfo.bounds(), paint);

            // Swap buffers for next iteration
            auto tmp = drawSurface;
            drawSurface = readSurface;
            readSurface = tmp;
            blurBuilder.child("input") = nullptr;
        }
        lastDrawTarget = readSurface;
    }

    drawSurface->flushAndSubmit();

    // do the final composition, with alpha blending to hide downscaling artifacts.
    {
        SkPaint paint;
        paint.setShader(lastDrawTarget->makeImageSnapshot()->makeShader(
                SkMatrix::MakeScale(kInverseInputScale)));
        paint.setFilterQuality(kLow_SkFilterQuality);
        paint.setAlpha(std::min(1.0f, (float)blurRadius / kMaxCrossFadeRadius) * 255);
        canvas->drawIRect(SkIRect::MakeWH(input->width(), input->height()), paint);
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android