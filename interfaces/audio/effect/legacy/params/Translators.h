/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>

#include "params/ParameterTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

// Typed translators, one per file family. Each mirrors the legacy <-> AIDL
// mapping that the framework's libaudiohal performs on the other side
// (frameworks/av/media/libaudiohal/impl/effectsAidlConversion/).
std::unique_ptr<ParameterTranslator> CreateEqualizerTranslator();
std::unique_ptr<ParameterTranslator> CreateBassBoostTranslator();
std::unique_ptr<ParameterTranslator> CreateVirtualizerTranslator();
std::unique_ptr<ParameterTranslator> CreatePresetReverbTranslator();
std::unique_ptr<ParameterTranslator> CreateEnvironmentalReverbTranslator();
std::unique_ptr<ParameterTranslator> CreateLoudnessEnhancerTranslator();
std::unique_ptr<ParameterTranslator> CreateDownmixTranslator();
std::unique_ptr<ParameterTranslator> CreateAecTranslator();
std::unique_ptr<ParameterTranslator> CreateNoiseSuppressionTranslator();
std::unique_ptr<ParameterTranslator> CreateAgc1Translator();
std::unique_ptr<ParameterTranslator> CreateAgc2Translator();
std::unique_ptr<ParameterTranslator> CreateVisualizerTranslator();

}  // namespace aidl::android::hardware::audio::effect::legacy
