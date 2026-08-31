#!/usr/bin/env python3
"""
Prove the transfer-mode quiesce handshake is real and load-bearing.

WHY. Transfer mode is what lets the upload path share storage with the playback
ring. That is only safe while NEITHER the audio thread nor the streamer can
still be touching it, and "we set a flag and they'll notice" is a race, not a
proof: the flag is raised and xfer_service() returns, with nothing establishing
that either thread has observed it before the first command starts writing.

The failure mode is an upload silently overwriting audio that is playing.
Nothing errors -- it just sounds wrong. This gate exists because that is
exactly the class of bug a comment cannot prevent.

Checks, all structural:
  1. Both acknowledgement flags exist.
  2. The audio thread sets its flag inside the branch where it emits silence.
  3. The streamer sets its flag inside the branch where it skips its pass.
  4. Both are CLEARED on entry to transfer mode, so a stale acknowledgement
     from a previous transfer cannot stand in for a fresh one.
  5. Command dispatch is gated on BOTH -- and gated wholesale, so a future
     command cannot forget to opt in.
  6. The gate cannot hang: it has its own timeout back to ordinary playback.

AND WHAT THE HANDSHAKE IS FOR. The upload verify scratch no longer has an
allocation of its own; it is the last loop-pin buffer. That is only sound
while the pins carry no residency claim across a transfer, so:

  7. The scratch is not a separate array any more -- if it comes back, the
     reclamation has been silently undone and the rest of this is theatre.
  8. xfer_scratch() really resolves into the PIN pool, not the read-ahead
     ring: the ring keeps publishing a slot it can no longer vouch for after
     an 'X' exit, which is why it was rejected as the donor.
  9. Both pins are dropped BEFORE transfer mode is entered, so no pin can
     claim bytes the upload is about to overwrite.
"""
import re
import sys

AUDIO = "g_xfer_audio_quiesced"
STREAM = "g_xfer_stream_quiesced"


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def block_after(src, anchor, lines=14):
    i = src.find(anchor)
    if i < 0:
        return None
    return "\n".join(src[i:].splitlines()[:lines])


def main():
    path = sys.argv[1]
    report = sys.argv[2] if len(sys.argv) > 2 else None
    src = open(path, encoding="utf-8", errors="replace").read()
    notes = []

    for flag in (AUDIO, STREAM):
        if f"static atomic_t {flag};" not in src:
            fail(f"{flag} is missing -- the handshake has been removed")
    notes.append("- both acknowledgement flags exist")

    # (2) the audio thread acknowledges where it emits silence.
    blk = block_after(src, "if (g_xfer_mode) {\n\t\t/* ACKNOWLEDGE, then silence.")
    if blk is None or AUDIO not in blk or "memset(s, 0, BLK_BYTES)" not in blk:
        fail("the audio thread does not acknowledge at the point it emits silence")
    notes.append("- the audio thread acknowledges where it silences the block")

    # (3) the streamer acknowledges where it skips.
    blk = block_after(src, "if (g_xfer_mode) {\n\t\t\t/* Same acknowledgement")
    if blk is None or STREAM not in blk or "continue;" not in blk:
        fail("the streamer does not acknowledge at the point it skips its pass")
    notes.append("- the streamer acknowledges where it skips its pass")

    # (4) cleared BEFORE the flag is raised, and (9) the pins dropped there
    # too -- one regex, because they are one invariant and drifting apart is
    # exactly the failure worth preventing.
    m = re.search(
        r"atomic_set\(&" + AUDIO + r", 0\);\s*\n\s*atomic_set\(&" + STREAM +
        r", 0\);(?P<between>.*?)g_xfer_mode = 1;", src, re.S)
    if not m:
        fail("the acknowledgements are not cleared before g_xfer_mode is "
             "raised -- a stale ack could stand in for a fresh one")
    notes.append("- both are cleared immediately before transfer mode is entered")

    if "stem_loop_pins_drop();" not in m.group("between"):
        fail("the loop pins are not dropped before transfer mode is entered "
             "-- the upload verify scratch IS a pin buffer, so a pin left "
             "claiming residency claims bytes the upload is about to overwrite")
    notes.append("- both loop pins are dropped before transfer mode is entered")

    # (5) dispatch is gated on both, wholesale.
    gate = src.find("if (!xfer_quiesced()) {")
    if gate < 0:
        fail("command dispatch is not gated on the handshake")
    helper = src.find("static inline bool xfer_quiesced(void)")
    if helper < 0:
        fail("xfer_quiesced() is missing")
    hb = block_after(src, "static inline bool xfer_quiesced(void)", 8)
    if AUDIO not in hb or STREAM not in hb or "&&" not in hb:
        fail("xfer_quiesced() does not require BOTH acknowledgements")
    cmd_read = src.find("if (ring_buf_get(&g_cdc_rx, &cmd, 1) != 1)")
    if cmd_read < 0 or gate > cmd_read:
        fail("the gate does not precede the command read, so a command can "
             "dispatch before both threads have acknowledged")
    notes.append("- dispatch is gated on BOTH acknowledgements, before any "
                 "command byte is consumed")

    # (6) and the gate cannot hang.
    gb = block_after(src, "if (!xfer_quiesced()) {", 20)
    if "g_xfer_mode = 0;" not in gb:
        fail("the quiesce gate has no escape -- a thread that never "
             "acknowledged would strand the device in transfer mode")
    notes.append("- the gate times out back to ordinary playback rather than hanging")

    # (7) the scratch is not its own allocation any more.
    if re.search(r"static\s+uint8_t\s+s_v11_verify_scratch\s*\[", src):
        fail("s_v11_verify_scratch is a separate 8192-byte array again -- the "
             "reclamation the handshake exists for has been undone")
    notes.append("- the upload verify scratch has no allocation of its own")

    # (8) and it resolves into the PIN pool, not the ring.
    sb = block_after(src, "static inline uint8_t *xfer_scratch(void)", 5)
    if sb is None:
        fail("xfer_scratch() is missing -- the transfer path has nowhere to "
             "verify into")
    if "g_stem_group_bufs" in sb:
        fail("xfer_scratch() aliases the READ-AHEAD RING. The mailbox keeps "
             "publishing the slot it last published and an 'X' exit reloads "
             "nothing, so playback resumes on bytes the upload overwrote")
    if "g_stem_loop_pin_bufs" not in sb:
        fail("xfer_scratch() does not resolve into the loop-pin pool, so the "
             "pin-drop on entry does not actually protect it")
    notes.append("- it is a loop-pin buffer, which has a 'nothing is resident' "
                 "state; the ring, which does not, was rejected as the donor")

    print("PASS: the transfer-mode quiesce handshake is wired and load-bearing")
    for n in notes:
        print("  " + n)
    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write("### Transfer-mode quiesce handshake\n\n")
            f.write("Upload and playback share storage only while both threads "
                    "have acknowledged that they stopped touching it:\n\n")
            f.write("\n".join(notes) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
