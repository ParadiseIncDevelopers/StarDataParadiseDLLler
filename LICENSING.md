# Native SDK licensing / Lisans kapsamı

The original StarDataParadise Native SDK, public C/C++ headers, examples,
tests and associated documentation are licensed under [MIT](LICENSE).
The original StarDataParadise wire-contract content carried in
`contracts/sdp-contract-v1.zip` is also MIT-covered. The archive is an
immutable protocol artifact; its bytes and checksum are unchanged.

The MIT grant permits commercial use, modification and redistribution while
retaining the copyright and permission notice. It applies only to material
the project authors have the right to license. Dependencies and other
separately identified material retain their own licenses and notices.

This license does not grant trademark or endorsement rights, or access to
separately offered Enterprise software, Cloud services, credentials or support.
It does not restrict commercial use of the MIT-covered code. Previously
granted rights remain intact.

## Third-party components

| Component | Separate terms / notice |
| --- | --- |
| OpenSSL | [Apache License 2.0](licenses/OpenSSL.txt) |
| Asio | [Boost Software License 1.0](https://github.com/chriskohlhoff/asio/blob/master/asio/LICENSE_1_0.txt) |
| nlohmann/json | [MIT](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT), with its own copyright notice |

[The install rules](cmake/InstallPackage.cmake) copy the project LICENSE and
this scope document, retain the existing OpenSSL notice, and copy the Asio
and nlohmann/json license files from the actual build dependencies. Preserve
those notices when redistributing the SDK and any bundled dependencies.
This list describes the direct libraries identified in the build; any other
redistributed runtime keeps its own terms.

## Türkçe özet

Özgün Native SDK kodu, C/C++ başlıkları ve örnekleri MIT lisanslıdır.
Telif ve izin bildirimi korunarak ticari kullanım, değiştirme ve yeniden
dağıtım mümkündür. OpenSSL, Asio ve nlohmann/json kendi lisanslarını korur.
