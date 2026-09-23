#!/usr/bin/env python3
"""
Patch upstream libpeer for WebRTC compliance and browser interoperability.
"""
import sys
import os

def patch_file(path, old, new):
    if not os.path.exists(path):
        print(f"Error: {path} not found")
        sys.exit(1)
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    if old not in content:
        if new in content:
            print(f"Already patched: {path}")
            return
        print(f"Error: target string not found in {path}")
        print(f"Looked for:\n{old[:100]}...")
        sys.exit(1)
    content = content.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"Patched {path}")

def main():
    if len(sys.argv) < 2:
        src = "build/libpeer-src"
    else:
        src = sys.argv[1]

    # 1. CMakeLists.txt: link order for sample
    cmakelists = os.path.join(src, "CMakeLists.txt")
    patch_file(cmakelists,
               'set(DEP_LIBS "srtp2" "usrsctp" "mbedtls" "mbedcrypto" "mbedx509" "cjson")',
               'set(DEP_LIBS "srtp2" "usrsctp" "mbedtls" "mbedx509" "mbedcrypto" "cjson")')

    # 2. src/CMakeLists.txt: install static archive to lib/
    src_cmakelists = os.path.join(src, "src/CMakeLists.txt")
    patch_file(src_cmakelists,
               'LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/',
               'ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/ LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/')

    # 3. address.c: implement addr_equal
    addr_c = os.path.join(src, "src/address.c")
    patch_file(addr_c,
"""int addr_equal(const Address* a, const Address* b) {
  // TODO
  return 1;
}""",
"""int addr_equal(const Address* a, const Address* b) {
  if (!a || !b) return 0;
  if (a->family != b->family) return 0;
  if (a->port != b->port) return 0;
  if (a->family == AF_INET) {
    return memcmp(&a->sin.sin_addr, &b->sin.sin_addr, sizeof(struct in_addr)) == 0;
  }
  if (a->family == AF_INET6) {
    return memcmp(&a->sin6.sin6_addr, &b->sin6.sin6_addr, sizeof(struct in6_addr)) == 0;
  }
  return 0;
}""")

    # 4. sdp.h: add mid and pt parameters
    sdp_h = os.path.join(src, "src/sdp.h")
    patch_file(sdp_h,
               "void sdp_append_h264(char* sdp);",
               "void sdp_append_h264(char* sdp, const char* mid, int pt);")
    patch_file(sdp_h,
               "void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel);",
               "void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel, const char* bundle_mid);")

    # 5. sdp.c: support custom mid, dynamic pt, and fmtp profile
    sdp_c = os.path.join(src, "src/sdp.c")
    old_sdp_append_h264 = """void sdp_append_h264(char* sdp) {
  sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 96");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtcp-fb:96 nack");
  sdp_append(sdp, "a=rtcp-fb:96 nack pli");
  sdp_append(sdp, "a=fmtp:96 profile-level-id=42e01f;level-asymmetry-allowed=1");
  sdp_append(sdp, "a=rtpmap:96 H264/90000");
  sdp_append(sdp, "a=ssrc:1 cname:webrtc-h264");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:1");
  sdp_append(sdp, "a=rtcp-mux");
}"""
    new_sdp_append_h264 = """void sdp_append_h264(char* sdp, const char* mid, int pt) {
  int use_pt = (pt > 0) ? pt : 96;
  const char* use_mid = (mid && mid[0]) ? mid : "0";
  sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF %d", use_pt);
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtcp-fb:%d nack", use_pt);
  sdp_append(sdp, "a=rtcp-fb:%d nack pli", use_pt);
  sdp_append(sdp, "a=fmtp:%d profile-level-id=42e01f;level-asymmetry-allowed=1;packetization-mode=1", use_pt);
  sdp_append(sdp, "a=rtpmap:%d H264/90000", use_pt);
  sdp_append(sdp, "a=ssrc:1 cname:webrtc-h264");
  sdp_append(sdp, "a=sendonly");
  sdp_append(sdp, "a=mid:%s", use_mid);
  sdp_append(sdp, "a=rtcp-mux");
}"""
    patch_file(sdp_c, old_sdp_append_h264, new_sdp_append_h264)

    old_sdp_create = """void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel) {
  char bundle[64];
  sdp_append(sdp, "v=0");
  sdp_append(sdp, "o=- 1495799811084970 1495799811084970 IN IP4 0.0.0.0");
  sdp_append(sdp, "s=-");
  sdp_append(sdp, "t=0 0");
  sdp_append(sdp, "a=msid-semantic: iot");
#if ICE_LITE
  sdp_append(sdp, "a=ice-lite");
#endif
  memset(bundle, 0, sizeof(bundle));

  strcat(bundle, "a=group:BUNDLE");

  if (b_datachannel) {
    strcat(bundle, " 0");
  }

  if (b_video) {
    strcat(bundle, " 1");
  }

  if (b_audio) {
    strcat(bundle, " 2");
  }

  sdp_append(sdp, bundle);
}"""
    new_sdp_create = """void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel, const char* bundle_mid) {
  char bundle[64];
  sdp_append(sdp, "v=0");
  sdp_append(sdp, "o=- 1495799811084970 1495799811084970 IN IP4 0.0.0.0");
  sdp_append(sdp, "s=-");
  sdp_append(sdp, "t=0 0");
  sdp_append(sdp, "a=msid-semantic: iot");
#if ICE_LITE
  sdp_append(sdp, "a=ice-lite");
#endif
  memset(bundle, 0, sizeof(bundle));
  const char* bmid = (bundle_mid && bundle_mid[0]) ? bundle_mid : (b_video ? "0" : "0");
  snprintf(bundle, sizeof(bundle), "a=group:BUNDLE %s", bmid);
  sdp_append(sdp, bundle);
}"""
    patch_file(sdp_c, old_sdp_create, new_sdp_create)

    # 6. dtls_srtp.c: case-insensitive fingerprint comparison
    dtls_c = os.path.join(src, "src/dtls_srtp.c")
    patch_file(dtls_c,
"""    if (strncmp(remote_fingerprint, dtls_srtp->actual_remote_fingerprint,
                DTLS_SRTP_FINGERPRINT_LENGTH) != 0) {""",
"""    if (strncasecmp(remote_fingerprint, dtls_srtp->actual_remote_fingerprint,
                    DTLS_SRTP_FINGERPRINT_LENGTH) != 0) {""")

    # 7. ice.c: parse candidates with or without candidate: prefix
    ice_c = os.path.join(src, "src/ice.c")
    patch_file(ice_c,
"""  if (strncmp("a=", candidate_start, strlen("a=")) == 0) {
    candidate_start += strlen("a=");
  }
  candidate_start += strlen("candidate:");""",
"""  if (strncmp("a=", candidate_start, strlen("a=")) == 0) {
    candidate_start += strlen("a=");
  }
  if (strncmp("candidate:", candidate_start, strlen("candidate:")) == 0) {
    candidate_start += strlen("candidate:");
  }""")

    # 8. agent.c: auto-nomination and pair update on valid incoming STUN
    agent_c = os.path.join(src, "src/agent.c")
    old_agent_stun = """void agent_process_stun_request(Agent* agent, StunMessage* stun_msg, Address* addr) {
  StunMessage msg;
  StunHeader* header;
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->local_upwd) == 0) {
        header = (StunHeader*)stun_msg->buf;
        memcpy(agent->transaction_id, header->transaction_id, sizeof(header->transaction_id));
        agent_create_binding_response(agent, &msg, addr);
        agent_socket_send(agent, addr, msg.buf, msg.size);
      }
      break;
    default:
      break;
  }
}"""
    new_agent_stun = """void agent_process_stun_request(Agent* agent, StunMessage* stun_msg, Address* addr) {
  StunMessage msg;
  StunHeader* header;
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->local_upwd) == 0) {
        header = (StunHeader*)stun_msg->buf;
        memcpy(agent->transaction_id, header->transaction_id, sizeof(header->transaction_id));
        agent_create_binding_response(agent, &msg, addr);
        agent_socket_send(agent, addr, msg.buf, msg.size);

        int found = 0;
        for (int i = 0; i < agent->remote_candidates_count; i++) {
          if (addr_equal(&agent->remote_candidates[i].addr, addr)) {
            found = 1;
            break;
          }
        }
        if (!found && agent->remote_candidates_count < (int)(sizeof(agent->remote_candidates)/sizeof(agent->remote_candidates[0]))) {
          IceCandidate* rc = &agent->remote_candidates[agent->remote_candidates_count++];
          ice_candidate_create(rc, agent->remote_candidates_count, ICE_CANDIDATE_TYPE_PRFLX, addr);
          agent_update_candidate_pairs(agent);
        }
        for (int i = 0; i < agent->candidate_pairs_num; i++) {
          if (addr_equal(&agent->candidate_pairs[i].remote->addr, addr)) {
            agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_SUCCEEDED;
            agent->nominated_pair = &agent->candidate_pairs[i];
            agent->selected_pair = agent->nominated_pair;
            break;
          }
        }
      }
      break;
    default:
      break;
  }
}"""
    patch_file(agent_c, old_agent_stun, new_agent_stun)

    # 9. peer_connection.c:
    pc_c = os.path.join(src, "src/peer_connection.c")

    patch_file(pc_c,
"""  uint32_t remote_assrc;
  uint32_t remote_vssrc;""",
"""  uint32_t remote_assrc;
  uint32_t remote_vssrc;
  char video_mid[32];
  int video_pt;""")

    old_sdp_parse = """    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {"""
    new_sdp_parse = """    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
      int pt_val = 0;
      if (sscanf(buf, "m=video %*d %*s %d", &pt_val) == 1) {
        pc->video_pt = pt_val;
      }
    } else if (strstr(buf, "m=audio")) {"""
    patch_file(pc_c, old_sdp_parse, new_sdp_parse)

    patch_file(pc_c,
"""    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      *ssrc = strtoul(val_start + 7, NULL, 10);
      LOGD("SSRC: %" PRIu32, *ssrc);
      ssrc = NULL;
    }""",
"""    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      *ssrc = strtoul(val_start + 7, NULL, 10);
      LOGD("SSRC: %" PRIu32, *ssrc);
      ssrc = NULL;
    }

    if (strstr(buf, "a=mid:")) {
      if (!pc->video_mid[0]) {
        sscanf(buf, "a=mid:%31s", pc->video_mid);
      }
    }""")

    old_pc_create_sdp = """  int sdp_audio = (pc->config.audio_codec != CODEC_NONE) && (sdp_type == SDP_TYPE_OFFER || pc->remote_assrc > 0);
  int sdp_video = (pc->config.video_codec != CODEC_NONE) && (sdp_type == SDP_TYPE_OFFER || pc->remote_vssrc > 0);

  sdp_create(pc->sdp, sdp_video, sdp_audio, pc->config.datachannel);"""
    new_pc_create_sdp = """  int sdp_audio = (pc->config.audio_codec != CODEC_NONE) && (sdp_type == SDP_TYPE_OFFER || pc->remote_assrc > 0);
  int sdp_video = (pc->config.video_codec != CODEC_NONE);

  sdp_create(pc->sdp, sdp_video, sdp_audio, pc->config.datachannel, pc->video_mid);"""
    patch_file(pc_c, old_pc_create_sdp, new_pc_create_sdp)

    patch_file(pc_c,
"""      case CODEC_H264:
        sdp_append_h264(pc->sdp);
        break;""",
"""      case CODEC_H264:
        sdp_append_h264(pc->sdp, pc->video_mid, pc->video_pt);
        break;""")

    old_pc_cand = """int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }

  LOGD("Add candidate: %s", candidate);
  agent->remote_candidates_count++;
  return 0;
}"""
    new_pc_cand = """int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }

  LOGD("Add candidate: %s", candidate);
  agent->remote_candidates_count++;
  agent_update_candidate_pairs(agent);
  return 0;
}"""
    patch_file(pc_c, old_pc_cand, new_pc_cand)

    print("All libpeer patches successfully applied!")

if __name__ == "__main__":
    main()
