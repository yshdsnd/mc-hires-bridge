#include "../bridge_status.h"
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
    puts("10 output-mode cases and status labels passed");return 0;
}
