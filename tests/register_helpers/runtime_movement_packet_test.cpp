#include "../../runtime/runtime_movement_packet.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
using Command = std::vector<uint8_t>;
bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
Command Move(uint8_t time, uint8_t sequence, uint8_t value) {
    return {3,6,time,sequence,value,0,0,0,0,0};
}
bool Append(std::array<uint8_t,256>& packet, const Command& command, bool compact) {
    const auto at = compact ? RuntimeMovementPacketReplacement(
        packet.data(), packet.size(), command.data(), command.size()) : 0;
    if (at) {
        std::copy(command.begin(),command.end(),packet.begin()+at);
        return true;
    }
    if (packet[1]+command.size()>254) return false;
    std::copy(command.begin(),command.end(),packet.begin()+2+packet[1]);
    packet[1] += static_cast<uint8_t>(command.size());
    return true;
}
std::vector<Command> Decode(const std::array<uint8_t,256>& packet) {
    std::vector<Command> commands;
    for (size_t at=2; at<size_t(packet[1])+2;) {
        const size_t end=at+4+packet[at+1];
        commands.emplace_back(packet.begin()+at,packet.begin()+end);
        at=end;
    }
    return commands;
}
struct Meaning {
    uint32_t movement_integral{};
    uint8_t final_vector{}, final_sequence{};
    std::vector<Command> other_commands;
    bool operator==(const Meaning& other) const {
        return movement_integral==other.movement_integral &&
            final_vector==other.final_vector && final_sequence==other.final_sequence &&
            other_commands==other.other_commands;
    }
};
// Independently interpret the time spent at each absolute input value. The
// zero-duration transitions must not change this integral or any button,
// rotation, timing-marker or unknown-command ordering/content.
Meaning Interpret(const std::vector<Command>& commands) {
    Meaning value;
    uint8_t time=0;
    for (const auto& c:commands) {
        value.movement_integral += uint32_t(c[2]-time)*value.final_vector;
        time=c[2];
        if (c[0]==3) value.final_vector=c[4];
        else value.other_commands.push_back(c);
        value.final_sequence=c[3];
    }
    value.movement_integral += uint32_t(128-time)*value.final_vector;
    return value;
}
}

int main() {
    bool passed=true;
    passed &= Check(RuntimeMovementPacketCaller(0x8243355C), "prediction caller accepted");
    passed &= Check(RuntimeMovementPacketCaller(0x8243309C), "retiring fixed-step caller accepted");
    for (uint32_t caller : {0u,0x824333D4u,0x82433098u,0x82433558u,0x8215409Cu,0x82589BF8u})
        passed &= Check(!RuntimeMovementPacketCaller(caller), "relative-time/unknown callers rejected");

    // Dense same-time callbacks reproduce the overflowing high-rate shape.
    // Lower input density must retain the same elapsed movement integral.
    for (int updates : {1,2,4,8}) {
        std::vector<Command> commands;
        uint8_t sequence=250;
        for (int t=0;t<128;t+=16) {
            for (int n=0;n<updates;++n)
                commands.push_back(Move(uint8_t(t),sequence++,uint8_t(n%2)));
            commands.push_back(Move(uint8_t(t),sequence++,1));
        }
        std::array<uint8_t,256> compacted{1,0}, original{1,0};
        bool old_fits=true;
        for (const auto& c:commands) {
            old_fits = Append(original,c,false) && old_fits;
            passed &= Check(Append(compacted,c,true), "dense equivalent vector states fit");
        }
        passed &= Check(Interpret(Decode(compacted))==Interpret(commands),
                        "time, final vector and sequence preserved across density/sequence wrap");
        if (updates>=4) passed &= Check(!old_fits, "fixture must exercise original packet-capacity failure");
    }
    {
        const std::vector<Command> commands{
            Move(0,1,1),Move(0,2,0),Move(0,3,1),
            {1,4,0,4,1,0,0,0}, // button edge is a barrier
            Move(0,5,0),Move(0,6,1),
            {2,6,0,7,9,8,7,6,5,4}, // rotation is a barrier
            Move(0,8,0),Move(1,9,1), // nonzero duration cannot merge
            {0,1,1,10,127}, // timing/ack marker is a barrier
            Move(1,11,0),Move(1,12,1),
            {17,1,2,13,55},Move(2,14,0)};
        std::array<uint8_t,256> packet{1,0};
        for (const auto& c:commands) passed &= Check(Append(packet,c,true), "mixed packet fits");
        passed &= Check(Interpret(Decode(packet))==Interpret(commands), "mixed command semantics preserved");
        passed &= Check(Decode(packet).size()==commands.size()-4, "only adjacent equal-time vectors combined");
    }
    {
        std::array<uint8_t,256> packet{1,0};
        // Exactly254 bytes, including an immediately superseded vector.
        // Construct a valid unknown-command prefix explicitly.
        for (int n=0;n<6;++n) { Command c(36,0);c[0]=9;c[1]=32;Append(packet,c,false); }
        Command c(28,0);c[0]=9;c[1]=24;Append(packet,c,false);
        Append(packet,Move(100,20,0),false);
        passed &= Check(packet[1]==254, "full packet fixture");
        passed &= Check(Append(packet,Move(100,21,1),true) && packet[1]==254,
                        "same-time replacement works at capacity without growing packet");
        passed &= Check(!Append(packet,Move(101,22,0),true), "later input retains capacity failure");
    }
    {
        std::array<uint8_t,256> packet{1,10,3,6,10,1};
        const auto c=Move(10,2,1);
        passed &= Check(RuntimeMovementPacketReplacement(packet.data(),12,c.data(),10)==2,
                        "bounded exact packet accepted");
        passed &= Check(!RuntimeMovementPacketReplacement(packet.data(),11,c.data(),10), "truncated packet rejected");
        passed &= Check(!RuntimeMovementPacketReplacement(packet.data(),12,c.data(),9), "truncated command rejected");
        packet[1]=255;
        passed &= Check(!RuntimeMovementPacketReplacement(packet.data(),256,c.data(),10), "invalid packet length rejected");
        packet[1]=10;packet[3]=32;
        passed &= Check(!RuntimeMovementPacketReplacement(packet.data(),256,c.data(),10), "invalid entry extent rejected");
        packet[3]=6;packet[0]=0;
        passed &= Check(!RuntimeMovementPacketReplacement(packet.data(),256,c.data(),10), "unsupported packet kind rejected");
    }
    {
        // V379 ring merge: only a same-time vector after a queued vector.
        const uint8_t vector_a[4] = {3, 6, 17, 9};
        const uint8_t vector_b[4] = {3, 6, 17, 0};
        const uint8_t later[4] = {3, 6, 18, 0};
        const uint8_t frame[4] = {0, 4, 17, 8};
        const uint8_t buttons[4] = {1, 4, 17, 0};
        passed &= Check(RuntimeMovementRingMerges(vector_a, vector_b),
                        "same-time vector merges into the newest queued vector");
        passed &= Check(!RuntimeMovementRingMerges(vector_a, later), "later vector is queued");
        passed &= Check(!RuntimeMovementRingMerges(frame, vector_b),
                        "a vector after a frame command is queued");
        passed &= Check(!RuntimeMovementRingMerges(vector_a, buttons), "buttons are queued");
        passed &= Check(!RuntimeMovementRingMerges(nullptr, vector_b), "missing entry rejected");
        passed &= Check(RuntimeMovementRingNewest(5, 2, 240) == 4 &&
                            RuntimeMovementRingNewest(0, 200, 240) == 239,
                        "newest entry wraps with the ring");
        passed &= Check(RuntimeMovementRingNewest(7, 7, 240) == -1 &&
                            RuntimeMovementRingNewest(240, 0, 240) == -1 &&
                            RuntimeMovementRingNewest(1, 0, 0) == -1,
                        "empty or malformed rings rejected");
    }
    return passed ? 0 : 1;
}
