#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <recompiler_config.h>

namespace {
bool Expect(bool condition, const char* name) {
    if (condition) return true;
    std::cerr << "FAILED: " << name << '\n';
    return false;
}
}

int main() {
    const auto path = std::filesystem::path(DARKNESS_SOURCE_ROOT) /
        "config" / "darkness_recomp_switch_correction.toml";

    RecompilerConfig config;
    config.Load(path.string());

    bool passed = true;
    const auto memsetOwnerIt = config.functions.find(0x82899950u);
    passed &= Expect(memsetOwnerIt != config.functions.end(),
                     "optimized memset fall-through owner exists");
    if (memsetOwnerIt != config.functions.end())
        passed &= Expect(memsetOwnerIt->second == 0xA0u,
                         "optimized memset owns both byte-tail blocks");

    const auto switchIt = config.switchTables.find(0x8233CD80u);
    passed &= Expect(switchIt != config.switchTables.end(), "dispatcher overlay exists");
    if (switchIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 6> expected{
            0x8233CDB4u, 0x8233CDD0u, 0x8233CDECu,
            0x8233CE08u, 0x8233CE24u, 0x8233CE40u,
        };
        passed &= Expect(switchIt->second.r == 5u, "dispatcher uses guest r5 index");
        passed &= Expect(switchIt->second.labels.size() == expected.size(), "dispatcher has six labels");
        if (switchIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(switchIt->second.labels[i] == expected[i], "dispatcher label ordering");
        }
    }

    const auto functionIt = config.functions.find(0x8233CD80u);
    passed &= Expect(functionIt != config.functions.end(), "dispatcher owner exists");
    if (functionIt != config.functions.end())
        passed &= Expect(functionIt->second == 0xE4u, "dispatcher owner ends before next function");

    const auto vectorSwitchIt = config.switchTables.find(0x820EC0E8u);
    passed &= Expect(vectorSwitchIt != config.switchTables.end(), "vector dispatcher overlay exists");
    if (vectorSwitchIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 6> expected{
            0x820EC1D4u, 0x820EC190u, 0x820EC1ACu,
            0x820EC140u, 0x820EC168u, 0x820EC118u,
        };
        passed &= Expect(vectorSwitchIt->second.r == 4u, "vector dispatcher uses guest r4 index");
        passed &= Expect(vectorSwitchIt->second.labels.size() == expected.size(),
                         "vector dispatcher has six labels");
        if (vectorSwitchIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(vectorSwitchIt->second.labels[i] == expected[i],
                                 "vector dispatcher label ordering");
        }
    }

    const auto hexSwitchIt = config.switchTables.find(0x8237AD88u);
    passed &= Expect(hexSwitchIt != config.switchTables.end(), "hex dispatcher overlay exists");
    if (hexSwitchIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 16> expected{
            0x8237AE58u, 0x8237AE50u, 0x8237AE48u, 0x8237AE40u,
            0x8237AE38u, 0x8237AE30u, 0x8237AE28u, 0x8237AE20u,
            0x8237AE18u, 0x8237AE10u, 0x8237AE08u, 0x8237AE00u,
            0x8237ADF8u, 0x8237ADF0u, 0x8237ADE8u, 0x8237ADE0u,
        };
        passed &= Expect(hexSwitchIt->second.r == 3u, "hex dispatcher uses guest r3 index");
        passed &= Expect(hexSwitchIt->second.labels.size() == expected.size(),
                         "hex dispatcher has sixteen labels");
        if (hexSwitchIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(hexSwitchIt->second.labels[i] == expected[i],
                                 "hex dispatcher label ordering");
        }
    }

    const auto hexFunctionIt = config.functions.find(0x8237AD88u);
    passed &= Expect(hexFunctionIt != config.functions.end(), "hex dispatcher owner exists");
    if (hexFunctionIt != config.functions.end())
        passed &= Expect(hexFunctionIt->second == 0xD8u,
                         "hex dispatcher owner ends before next function");

    const auto floatSwitchIt = config.switchTables.find(0x822FDF34u);
    passed &= Expect(floatSwitchIt != config.switchTables.end(),
                     "floating-point dispatcher overlay exists");
    if (floatSwitchIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 5> expected{
            0x822FDF78u, 0x822FDF94u, 0x822FDFDCu,
            0x822FDF60u, 0x822FE024u,
        };
        passed &= Expect(floatSwitchIt->second.r == 6u,
                         "floating-point dispatcher uses guest r6 index");
        passed &= Expect(floatSwitchIt->second.labels.size() == expected.size(),
                         "floating-point dispatcher has five labels");
        if (floatSwitchIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(floatSwitchIt->second.labels[i] == expected[i],
                                 "floating-point dispatcher label ordering");
        }
    }

    const auto pairedFirstIt = config.switchTables.find(0x821C495Cu);
    passed &= Expect(pairedFirstIt != config.switchTables.end(),
                     "first paired dispatcher overlay exists");
    if (pairedFirstIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 5> expected{
            0x821C4988u, 0x821C4A94u, 0x821C4988u,
            0x821C49B0u, 0x821C4988u,
        };
        passed &= Expect(pairedFirstIt->second.r == 24u,
                         "first paired dispatcher uses bounded guest r24");
        passed &= Expect(pairedFirstIt->second.labels.size() == expected.size(),
                         "first paired dispatcher has five labels");
        if (pairedFirstIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(pairedFirstIt->second.labels[i] == expected[i],
                                 "first paired dispatcher label ordering");
        }
    }

    const auto pairedSecondIt = config.switchTables.find(0x821C4B00u);
    passed &= Expect(pairedSecondIt != config.switchTables.end(),
                     "second paired dispatcher overlay exists");
    if (pairedSecondIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 5> expected{
            0x821C4B2Cu, 0x821C4BA8u, 0x821C4B88u,
            0x821C4B54u, 0x821C4B88u,
        };
        passed &= Expect(pairedSecondIt->second.r == 24u,
                         "second paired dispatcher uses bounded guest r24");
        passed &= Expect(pairedSecondIt->second.labels.size() == expected.size(),
                         "second paired dispatcher has five labels");
        if (pairedSecondIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(pairedSecondIt->second.labels[i] == expected[i],
                                 "second paired dispatcher label ordering");
        }
    }

    const auto pairedOwnerIt = config.functions.find(0x821C45C8u);
    passed &= Expect(pairedOwnerIt != config.functions.end(),
                     "paired dispatcher owner exists");
    if (pairedOwnerIt != config.functions.end())
        passed &= Expect(pairedOwnerIt->second == 0x5F4u,
                         "paired dispatcher owner ends after final tail transfer");

    const auto flagSwitchIt = config.switchTables.find(0x82589A5Cu);
    passed &= Expect(flagSwitchIt != config.switchTables.end(),
                     "flag-conversion dispatcher overlay exists");
    if (flagSwitchIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 7> expected{
            0x82589A90u, 0x82589AA0u, 0x82589AB0u, 0x82589B28u,
            0x82589AB8u, 0x82589B28u, 0x82589AE8u,
        };
        passed &= Expect(flagSwitchIt->second.r == 4u,
                         "flag-conversion dispatcher uses bounded guest r4");
        passed &= Expect(flagSwitchIt->second.labels.size() == expected.size(),
                         "flag-conversion dispatcher has seven labels");
        if (flagSwitchIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(flagSwitchIt->second.labels[i] == expected[i],
                                 "flag-conversion dispatcher label ordering");
        }
    }

    const auto flagOwnerIt = config.functions.find(0x82589A58u);
    passed &= Expect(flagOwnerIt != config.functions.end(),
                     "flag-conversion dispatcher owner exists");
    if (flagOwnerIt != config.functions.end())
        passed &= Expect(flagOwnerIt->second == 0xD8u,
                         "flag-conversion owner ends before next prologue");

    const auto renderFormatIt = config.switchTables.find(0x827CC178u);
    passed &= Expect(renderFormatIt != config.switchTables.end(),
                     "render-format dispatcher overlay exists");
    if (renderFormatIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 7> expected{
            0x827CC1ACu, 0x827CC22Cu, 0x827CC1D4u, 0x827CC22Cu,
            0x827CC21Cu, 0x827CC22Cu, 0x827CC1F8u,
        };
        passed &= Expect(renderFormatIt->second.r == 4u,
                         "render-format dispatcher uses bounded guest r4");
        passed &= Expect(renderFormatIt->second.labels.size() == expected.size(),
                         "render-format dispatcher has seven labels");
        if (renderFormatIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(renderFormatIt->second.labels[i] == expected[i],
                                 "render-format dispatcher label ordering");
        }
    }

    const auto typeClassIt = config.switchTables.find(0x821971A8u);
    passed &= Expect(typeClassIt != config.switchTables.end(),
                     "type-classification dispatcher overlay exists");
    if (typeClassIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 23> expected{
            0x8219721Cu, 0x8219721Cu, 0x8219721Cu, 0x8219721Cu,
            0x8219721Cu, 0x8219721Cu, 0x8219722Cu, 0x8219722Cu,
            0x82197224u, 0x82197224u, 0x8219722Cu, 0x8219722Cu,
            0x8219722Cu, 0x8219722Cu, 0x8219722Cu, 0x8219722Cu,
            0x8219722Cu, 0x8219722Cu, 0x82197224u, 0x8219722Cu,
            0x8219722Cu, 0x8219722Cu, 0x82197224u,
        };
        passed &= Expect(typeClassIt->second.r == 3u,
                         "type-classification dispatcher uses bounded guest r3");
        passed &= Expect(typeClassIt->second.labels.size() == expected.size(),
                         "type-classification dispatcher has 23 labels");
        if (typeClassIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(typeClassIt->second.labels[i] == expected[i],
                                 "type-classification dispatcher label ordering");
        }
    }

    const auto enumRemapIt = config.switchTables.find(0x82113F00u);
    passed &= Expect(enumRemapIt != config.switchTables.end(),
                     "enum-remap dispatcher overlay exists");
    if (enumRemapIt != config.switchTables.end()) {
        passed &= Expect(enumRemapIt->second.r == 3u,
                         "enum-remap dispatcher uses bounded guest r3");
        passed &= Expect(enumRemapIt->second.labels.size() == 59u,
                         "enum-remap dispatcher has 59 labels");
        if (enumRemapIt->second.labels.size() == 59u) {
            passed &= Expect(enumRemapIt->second.labels.front() == 0x82114004u,
                             "enum-remap first label");
            passed &= Expect(enumRemapIt->second.labels[55] == 0x821141BCu,
                             "enum-remap final unique leaf label");
            passed &= Expect(enumRemapIt->second.labels[57] == 0x82114184u,
                             "enum-remap duplicate label ordering");
            passed &= Expect(enumRemapIt->second.labels.back() == 0x82114194u,
                             "enum-remap final label");
        }
    }

    const auto renderFormatOwnerIt = config.functions.find(0x827CC178u);
    passed &= Expect(renderFormatOwnerIt != config.functions.end(),
                     "render-format owner exists");
    if (renderFormatOwnerIt != config.functions.end())
        passed &= Expect(renderFormatOwnerIt->second == 0xBCu,
                         "render-format owner ends before padding");
    const auto typeClassOwnerIt = config.functions.find(0x821971A8u);
    passed &= Expect(typeClassOwnerIt != config.functions.end(),
                     "type-classification owner exists");
    if (typeClassOwnerIt != config.functions.end())
        passed &= Expect(typeClassOwnerIt->second == 0x8Cu,
                         "type-classification owner ends before padding");
    const auto enumRemapOwnerIt = config.functions.find(0x82113F00u);
    passed &= Expect(enumRemapOwnerIt != config.functions.end(),
                     "enum-remap owner exists");
    if (enumRemapOwnerIt != config.functions.end())
        passed &= Expect(enumRemapOwnerIt->second == 0x2CCu,
                         "enum-remap owner ends before padding");

    const auto nameLookupIt = config.switchTables.find(0x82310F50u);
    passed &= Expect(nameLookupIt != config.switchTables.end(),
                     "name-lookup dispatcher overlay exists");
    if (nameLookupIt != config.switchTables.end()) {
        passed &= Expect(nameLookupIt->second.r == 3u,
                         "name-lookup dispatcher uses bounded guest r3");
        passed &= Expect(nameLookupIt->second.labels.size() == 52u,
                         "name-lookup dispatcher has 52 labels");
        if (nameLookupIt->second.labels.size() == 52u) {
            passed &= Expect(nameLookupIt->second.labels.front() == 0x82311038u,
                             "name-lookup first label");
            passed &= Expect(nameLookupIt->second.labels[31] == 0x823110F8u,
                             "name-lookup default run ends at index 31");
            passed &= Expect(nameLookupIt->second.labels[32] == 0x82311044u,
                             "name-lookup named run begins at index 32");
            passed &= Expect(nameLookupIt->second.labels[48] == 0x823110C8u,
                             "name-lookup final named run begins at index 48");
            passed &= Expect(nameLookupIt->second.labels.back() == 0x823110ECu,
                             "name-lookup final label");
        }
    }
    const auto nameLookupOwnerIt = config.functions.find(0x82310F50u);
    passed &= Expect(nameLookupOwnerIt != config.functions.end(),
                     "name-lookup owner exists");
    if (nameLookupOwnerIt != config.functions.end())
        passed &= Expect(nameLookupOwnerIt->second == 0x1B0u,
                         "name-lookup owner ends before next prologue");

    const auto vectorMathIt = config.switchTables.find(0x826F9554u);
    passed &= Expect(vectorMathIt != config.switchTables.end(),
                     "vector-math dispatcher overlay exists");
    if (vectorMathIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 15> expected{
            0x826F95A8u, 0x826F95A8u, 0x826F95A8u, 0x826F99D0u,
            0x826F9BE4u, 0x826F9DDCu, 0x826F97BCu, 0x826F95A8u,
            0x826F9FF0u, 0x826FA1B8u, 0x826F95A8u, 0x826FA3B0u,
            0x826FA3B0u, 0x826FA5ECu, 0x826FA5ECu,
        };
        passed &= Expect(vectorMathIt->second.r == 4u,
                         "vector-math dispatcher uses bounded guest r4");
        passed &= Expect(vectorMathIt->second.labels.size() == expected.size(),
                         "vector-math dispatcher has 15 labels");
        if (vectorMathIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(vectorMathIt->second.labels[i] == expected[i],
                                 "vector-math dispatcher label ordering");
        }
    }
    const auto vectorMathOwnerIt = config.functions.find(0x826F9258u);
    passed &= Expect(vectorMathOwnerIt != config.functions.end(),
                     "vector-math owner exists");
    if (vectorMathOwnerIt != config.functions.end())
        passed &= Expect(vectorMathOwnerIt->second == 0x15D0u,
                         "vector-math owner ends before next prologue");

    const auto geometrySecondIt = config.switchTables.find(0x82982E40u);
    passed &= Expect(geometrySecondIt != config.switchTables.end(),
                     "second geometry dispatcher overlay exists");
    if (geometrySecondIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 21> expected{
            0x82982EACu, 0x82982EACu, 0x82983008u, 0x82982EACu,
            0x82982EACu, 0x82982F94u, 0x82982EACu, 0x82982F20u,
            0x82982EACu, 0x82982EACu, 0x82983008u, 0x82983008u,
            0x82982EACu, 0x82982EACu, 0x82983008u, 0x82983008u,
            0x82983008u, 0x82983008u, 0x82982EACu, 0x82982EACu,
            0x82982EACu,
        };
        passed &= Expect(geometrySecondIt->second.r == 19u,
                         "second geometry dispatcher uses bounded guest r19");
        passed &= Expect(geometrySecondIt->second.labels.size() == expected.size(),
                         "second geometry dispatcher has 21 labels");
        if (geometrySecondIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(geometrySecondIt->second.labels[i] == expected[i],
                                 "second geometry dispatcher label ordering");
        }
    }

    const auto curveDispatchIt = config.switchTables.find(0x8272F4F8u);
    passed &= Expect(curveDispatchIt != config.switchTables.end(),
                     "curve dispatcher overlay exists");
    if (curveDispatchIt != config.switchTables.end()) {
        constexpr std::array<uint32_t, 7> expected{
            0x8272F52Cu, 0x8272F52Cu, 0x8272F604u, 0x8272F6C4u,
            0x8272F73Cu, 0x8272F77Cu, 0x8272F5CCu,
        };
        passed &= Expect(curveDispatchIt->second.r == 28u,
                         "curve dispatcher uses bounded guest r28");
        passed &= Expect(curveDispatchIt->second.labels.size() == expected.size(),
                         "curve dispatcher has seven labels");
        if (curveDispatchIt->second.labels.size() == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i)
                passed &= Expect(curveDispatchIt->second.labels[i] == expected[i],
                                 "curve dispatcher label ordering");
        }
    }

    return passed ? 0 : 1;
}
