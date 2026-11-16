#include "bridge_status.h"
#include <cstdio>
int main(){
    struct Case{const char* json;bool expected;};
    Case cases[]={
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default"}}}})",true},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"wasapi"}}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"asio"}}}})",false},
        {R"({"mode":"default","private":{"other":{"mode":"default"}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"unknown"}}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":null}}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default","mode":"wasapi"}}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default")",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"wasapi":{"deviceName":"a\"{x}"},"mode":"default","format":{"quantizationBits":24}}}}})",true},
        {"",false},
    };
    for(size_t i=0;i<_countof(cases);++i)if(bridgeStatus::defaultMode(cases[i].json)!=cases[i].expected){fprintf(stderr,"output-mode case %zu failed\n",i);return 1;}
    if(wcscmp(bridgeStatus::label(bridgeStatus::State::Ready),L"HR-BR RDY")||wcscmp(bridgeStatus::label(bridgeStatus::State::On),L"HR-BR ON")||wcscmp(bridgeStatus::label(bridgeStatus::State::Off),L"HR-BR OFF"))return 2;
    const Case dseeCases[]={
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default"}},"player":{"dsee-state":true}}})",true},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default"}},"player":{"dsee-state":false}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default"}},"player":{"dsee-state":"true"}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"wasapi"}},"player":{"dsee-state":true}}})",false},
        {R"({"private":{"audio-output-device":{"output-devices":{"mode":"default"}}}})",false},
    };
    for(size_t i=0;i<_countof(dseeCases);++i) {
        if(bridgeStatus::dseeSharedEnabled(dseeCases[i].json)!=dseeCases[i].expected) {
            fprintf(stderr,"DSEE preference case %zu failed\n",i);
            return 3;
        }
    }
    const auto details=bridgeStatus::tooltipText({
        L"ALAC",{44100,16,2},{176400,24,2},bridgeStatus::Route::Dsee
    });
    if(details.find(L"ALAC  44.1 kHz / 16 bit / 2 ch")==std::wstring::npos ||
       details.find(L"DirectSound  176.4 kHz / 24 bit / 2 ch")==std::wstring::npos ||
       bridgeStatus::formatText({48000,24,2})!=L"48 kHz / 24 bit / 2 ch")return 4;
    if (bridgeConfig::parseDseeMultiplierFromJson(R"({"dsee-multiplier": 4})") != 4 ||
        bridgeConfig::parseDseeMultiplierFromJson(R"({"dsee-multiplier": 2})") != 2 ||
        bridgeConfig::parseDseeMultiplierFromJson(R"({"dsee-multiplier": "invalid"})") != 2 ||
        bridgeConfig::parseDseeMultiplierFromJson("") != 2) return 5;
    puts("Output-mode, DSEE preference, status label, and tooltip checks passed");return 0;
}
