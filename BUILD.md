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
python tests/package_integration.py --package out/packages/StarDataParadiseDLLler-0.4.1-win-x64.zip --server <Server.exe> --openssl <openssl.exe> --scratch out/integration
```

Script geçici yerel TLS sertifikaları üretir ve ayrı bir Windows instance'ı loopback üzerinde çalıştırır. Örnek tüketiciyi ZIP'ten açılan CMake paketi ile derler ve bağlantı, capability, SQL unavailable, ret, timeout, iptal, bağlantı kaybı ve kapanışı doğrular. Etap 12 Server ile `--sql` kullanıldığında ayrı bir veri deposu başlatır ve tipli CRUD, batch, savepoint, sonuç iptali, kayıp commit yanıtı ve restart sonrası outcome kontrollerini de çalıştırır.

Etap 15 Server ile `--security` de eklenebilir. Ayrı TLS fixture'ı, kullanıcı ve rol değişikliklerini, yeniden kullanımda reset, yetki revoke, parola yenileme ve oturum kapatmayı doğrular.

Etap 17 Server ile `--plsql` kurulu paket üzerinden OUT, adlandırılmış cursor setleri ve iç içe RECORD/COLLECTION gidiş-dönüşünü doğrular.

Etap 18 Server ile `--debugger` gerçek bir kontrol bağlantısı ve ayrı bir hedef bağlantıyla breakpoint, watch, step, ölçüm, isolated routine test ve kontrol bağlantısı kapanırken rollback’i doğrular.

Etap 19 Server ile `--history`, Native LOG, DIFF ve READ çağrılarını, primary-key değişiminde satır kimliğini, History yetkisini ve revision pin/retention davranışını ayrı bir TLS fixture’ında doğrular.

Etap 20 Server ile `--restore`, silinen satırı aynı kimlikle geri almayı, eskimiş önizleme reddini, ayrı TLS bağlantısından token replay ve bağımsız alan güncellemesini koruyan REVERT’i doğrular.

Etap 22 Server ile `--decision`, Native kural oluşturma, doğrulama ve etkinleştirme çağrılarını, salt okunur açıklamayı, WHATIF ve RECHECK’i ve iki sürümün aynı örneklerdeki etkisini doğrular. Gerçek SQL yazımı etkin kuralı kullanır.

Etap 23 Server ile `--jobs`, Native iş talebini yeni TLS bağlantısından izler, sonucu bağımsız hesapla doğrular ve çalışan worker’ın gerçek ilerleme ve iptal durumlarını denetler. Ayrı senaryolarda rollback, sahiplik ve kalıcı takvim çalıştırması doğrulanır.

Etap 28 Server ile --ml seçeneği kurulu SDK üzerinden veri kümesi oluşturma, model eğitimi ve değerlendirme, tahmin, cihaz raporu ve model sürümü yaşam döngüsünü doğrular.

Etap 29 Server ile --backup seçeneği kurulu SDK aracılığıyla yedek işini, kalıcı durum okumayı, katalog ve doğrulamayı, yeni hedefe geri yüklemeyi ve arşiv silmeyi doğrular.

Etap 30 Server ile --admin seçeneği kurulu SDK’dan instance durumu, ölçülen sağlık, config önizleme ve uygulama, taranmış destek paketi ve kalıcı bakım işini doğrular.

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
