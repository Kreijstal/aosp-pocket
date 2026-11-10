LLVM-10.txt is the unmodified LLVM 10 license, including LLVM exceptions:
https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-10.0.1/llvm/LICENSE.TXT
SHA-256: 8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee

The Android 12L SwiftShader checkout vendors LLVM 10 source but omits its root
license text. The graphics fixture bundles this text, the vendored LLVM System
Interface Library notice, SwiftShader and ASTC licenses, and the NDK notice for
the statically linked C++ runtime. Source revisions are in sources.lock.json.
