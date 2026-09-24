# SDP extension regression

`tst_sdpextensions` builds the application's common-c source list with the
parent-owned SDP wrapper. It compares generated payloads with the unchanged
generator across Sunshine/GFE versions, negotiated codec profiles, all decoder
recovery capability combinations, and encryption settings. Unsupported cases
must be byte-for-byte identical; supported cases may add exactly one attribute
and must return the correct payload length.

Build out of tree with `qmake <repo>/tests/common/common.pro`, then `nmake`
(Windows) or `make` (Linux/macOS). On Windows run
`release/tst_sdpextensions.exe` with the deployed OpenSSL runtime DLLs on PATH.
The suite is also included by `tests/tests.pro` with `CONFIG+=tests`.

Run this when updating the common-c submodule: the wrapper depends on its
internal SDP entry point, negotiated state, and attribute/tail layout. These
checks do not establish whether a live host encoder uses Intra Refresh.
