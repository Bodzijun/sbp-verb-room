#pragma once
#include "RoomLibrary.h"
#include <cstdlib>
#include <stdexcept>
namespace verbp {
// Exhaustive checks remain available before releases. Default smoke coverage spans
// 7.36 m3 booth, 168 m3 studio, 26817.6 m3 hall, and the heaviest machine setup.
inline bool selectedTestRoom(const BuiltinRoom& room) {
    static const bool all=[] {
        const char* value=std::getenv("VERB_STAGE2_ROOMS");
        const std::string mode=value?value:"key";
        if(mode!="key" && mode!="all")throw std::runtime_error("VERB_STAGE2_ROOMS must be key or all");
        return mode=="all";
    }();
    return all || room.name=="Vocal booth (dead)" || room.name=="Studio live room"
        || room.name=="VSL Grand hall - audience (32 stereo groups)"
        || room.name=="MotorSynth + EngineSim (21 sources)";
}
}
