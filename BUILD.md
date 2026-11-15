# StarDataParadiseDLLler derleme, test ve paket

Windows x64 ve C++20. Visual Studio 2026 (v145), Windows SDK 10.0.26100.0 ve CMake 4.2.2 kullanılır. DLL MSVC dinamik çalışma zamanına bağlanır. OpenSSL 3.5.9 x64 statik kütüphaneleri gerekir.

```powershell
cmake --preset windows-x64 -DOPENSSL_ROOT_DIR="C:/deps/openssl-3.5.9"
cmake --build --preset windows-x64
ctest --preset windows-x64
cpack --config out/build/windows-x64/CPackConfig.cmake -C Release -B out/packages
```

CMake, bağımlılık kaynaklarını ve Server v1 sözleşmesini SHA-256 ile doğrular. Çevrimdışı çalışma için `-DSDP_JSON_ARCHIVE=...` ve `-DSDP_ASIO_ARCHIVE=...` kullan. `ci.ps1` bu seçenekleri `-OpenSslRoot`, `-JsonArchive`, `-AsioArchive` olarak alır. Çıktılar `out/build/windows-x64/Release/` altında. ZIP, DLL, import library, header ve CMake config hedefini içerir.

## Paket tüketicisi ve tanılama

```powershell
python tests/package_integration.py --package out/packages/StarDataParadiseDLLler-0.1.0-win-x64.zip --server <Server.exe> --openssl <openssl.exe> --scratch out/integration
```

Script geçici yerel TLS sertifikaları üretir ve ayrı bir Windows instance'ı loopback üzerinde çalıştırır. Örnek tüketiciyi ZIP'ten açılan CMake paketi ile derler ve bağlantı, capability, SQL unavailable, ret, timeout, iptal, bağlantı kaybı ve kapanışı doğrular.

MSVC AddressSanitizer için ayrı bir derleme dizini ve `-DSDP_ADDRESS_SANITIZER=ON` kullan. Visual Studio ASan x64 runtime dizinini test sürecinin PATH değişkenine ekle. Örnek tüketici de `package_integration.py --asan` ile enstrümante edilir. ASan paketi tanılama içindir.

## Mevcut Visual Studio çözümü

`contracts/README.md` dosyasındaki SHA-256 ile ZIP'i doğrulayıp aç. `SDP_CONTRACT_ROOT` çıkarılan `cpp` klasörünün üst dizinine, `SDP_JSON_ROOT` JSON arşiv köküne, `SDP_ASIO_ROOT` Asio arşiv köküne ve `OPENSSL_ROOT_DIR` OpenSSL kurulumuna işaret eder. `StarDataParadiseDLLler.slnx` içinde x64 seç.

## Bağımlılıklar ve lisans

- CMake 4.2.2, BSD-3-Clause. [Kitware](https://github.com/Kitware/CMake/tree/v4.2.2)
- MSVC v145 ve Windows SDK 10.0.26100.0, Microsoft lisans koşulları. [Çalışma zamanı](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
- nlohmann/json 3.12.0, MIT. [Sürüm](https://github.com/nlohmann/json/releases/tag/v3.12.0)
- Asio 1.38.2, BSL-1.0. [Sürüm](https://github.com/chriskohlhoff/asio/tree/asio-1-38-2)
- OpenSSL 3.5.9, Apache-2.0. [Sürüm](https://github.com/openssl/openssl/releases/tag/openssl-3.5.9)

API sözleşmesi [ABI.md](ABI.md) dosyasında.
