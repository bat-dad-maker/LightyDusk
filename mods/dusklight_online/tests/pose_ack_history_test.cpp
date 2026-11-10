#include "dusklight_online/net/pose_ack_history.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace dusklight_online::net;
void require(bool ok) { if (!ok) throw std::runtime_error("unsafe shared pose baseline"); }
int main() {
    PoseAckHistory a{10,12}, b{10,13}, c{10,11};
    std::array<const PoseAckHistory*,3> peers{&a,&b,&c};
    require(common_pose_ack(peers)==10); // min(latest ACK) incorrectly selects 11
    c.clear(); require(common_pose_ack(peers)==0); // baseline reset/new recipient
    remember_pose_ack(c,14); require(common_pose_ack(peers)==0);
    remember_pose_ack(a,14); remember_pose_ack(b,14);
    require(common_pose_ack(peers)==14);
    remember_pose_ack(a,12); require(common_pose_ack(peers)==14); // stale/duplicate ACK
    std::array<const PoseAckHistory*,1> single{&a};
    require(common_pose_ack(single)==14);
    for (uint32_t n=15;n<1000;++n) remember_pose_ack(a,n);
    require(a.size()==300 && common_pose_ack(peers)==0); // bounded, expired baseline
    require(common_pose_ack({})==0);
    std::cout << "Exact shared ACK, loss gaps, reset, reordering and bounds passed\n";
}
