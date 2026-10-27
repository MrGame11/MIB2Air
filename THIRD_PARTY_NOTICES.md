# Protocol references

The wireless Android Auto bootstrap in `native/aa-bootstrap/aa_bootstrap.c` and
`native/waa-spp/aa_boot_sm.c` uses message IDs, framing, field numbers and enum
values documented by [WirelessAndroidAutoDongle](https://github.com/nisargjhaveri/WirelessAndroidAutoDongle)
by Nisarg Jhaveri. The reference exchange is in `bluetoothProfiles.cpp`; the
outgoing message schemas are `WifiStartRequest.proto` and `WifiInfoResponse.proto`
under `aa_wireless_dongle/package/aawg/src/` (schemas in `proto/`).

The saved reference files match these upstream Git blobs exactly:

| File | Git blob SHA-1 |
| --- | --- |
| bluetoothProfiles.cpp | e6f5c6e23aba3d13bb36f85d04630886c642d292 |
| WifiStartRequest.proto | 8c63d82fe8cf6a14736343660ccf85fcca688871 |
| WifiInfoResponse.proto | 9389f5f55070f2843b9d44d421eb69241492a0a5 |

Reference tree: `ef645221ec28e87361391b9bdcf27e31ff48cb6f`.
This project implements a bounded C wire codec and a separate state machine.
It does not bundle the upstream C++ implementation, generated `.pb.h`/`.pb.cc`
files, or the Google Protocol Buffers runtime. Opaque incoming message payloads
are not interpreted as a successful connection; TCP acceptance completes bootstrap.

The upstream MIT notice is retained below for these protocol references.
The rest of this project is licensed under [LICENSE](LICENSE).

## WirelessAndroidAutoDongle — MIT License

Copyright (c) 2023 Nisarg Jhaveri

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
