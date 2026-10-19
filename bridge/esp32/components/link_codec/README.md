# link_codec

Link's binary frames (Link spec 8) and base64 for the `_b64` fields of the
control plane.

| File | What it does | Portable |
|------|--------------|----------|
| `include/link_frames.h`, `link_frames.c` | Decodes the frames clients send (CMD requests, FWDATA chunks) and encodes the ones the hub sends (CMD responses, NOTIFY, POLL, FWDATA acks, RESYNC). | yes |
| `include/link_base64.h`, `link_base64.c` | Standard base64 encoding, for `device.snapshot`. | yes |

Public API: `link_frame_type()`, `link_frame_tag()`,
`link_decode_cmd_request()`, `link_decode_fwdata()`, the
`link_encode_*()` functions, `link_base64_encode()` and
`link_base64_encoded_length()`.

Tests: `test/host/test_link_frames.c`, against the byte-exact frames in
`fixtures/frames/`, and `test/host/test_link_base64.c`, against the RFC 4648
test vectors.
