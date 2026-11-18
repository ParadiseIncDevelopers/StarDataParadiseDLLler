# StarDataParadise Native SDK

Windows x64 uygulamalarını TLS üzerinden StarDataParadise Server’ın wire v1 protokolüne bağlayan C ve C++ SDK’sı. 0.4.1 sürümü C ABI 1’i kullanır. C kitaplığı bağlantı, tipli SQL, işlemler ve yönetim çağrılarını sunar. C++20 başlığı bağlantı, ifade, transaction ve sonuç tamponlarının sahipliğini yönetir.

## Paketi kullanma

Windows x64 Release arşivi `StarDataParadiseDLLler-0.4.1-win-x64.zip` adını taşır. SHA-256 değerini dağıtım kaydıyla karşılaştırın ve ZIP’i açın. Pakette DLL, import library, C ve C++ başlıkları, CMake yapılandırması, wire sözleşmesi ve C++ örneği bulunur. Tüketici için MSVC x64 dinamik çalışma zamanı gerekir.

```cmake
find_package(StarDataParadiseDLLler 0.4.1 EXACT CONFIG REQUIRED)
target_link_libraries(my_client PRIVATE StarDataParadise::StarDataParadiseDLLler)
```

CMake yapılandırmasında `CMAKE_PREFIX_PATH` değerini ZIP’in açıldığı köke verin. Çalıştırırken `bin/StarDataParadiseDLLler.dll` dosyasını EXE’nin yanında veya uygulamanın güvenilir DLL arama yolunda bulundurun.

Depodaki [C++ örneği](examples/cpp/main.cpp) sağlık, oturum ve capability bilgilerini okur. Örneği açılmış Release paketiyle derlemek için depo kökünden şu komutları çalıştırın. Adres, port, sertifika adı, CA dosyası ve kullanıcıyı kendi test örneğinize göre girin. Örnek parolayı stdin’in ilk satırından okur.

```powershell
cmake -S examples/cpp -B out/native-example -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH="C:/SDP/native-sdk"
cmake --build out/native-example --config Release
$env:SDP_PASSWORD | .\out\native-example\Release\native_example.exe 127.0.0.1 7443 localhost C:\SDP\lab\ca.pem local-admin
```

## API ve bağlantı yaşam döngüsü

- C’de `sdp_open_options` içindeki `struct_size` ve `abi_version` alanlarını doldurun. Yanıt tamponlarını `sdp_buffer_free` ile bırakın. Açık ifade ve okuyucuları bağlantıyı kapatmadan önce bitirin.
- Aynı bağlantıda ağ çağrılarını sıralayın. Paralel işçiler için ayrı bağlantılar kullanın.
- Sunucu desteğini `sdp_get_capabilities` ile okuyun. Veri kümesi, model, History, Decision, yedek ve yönetim çağrıları sunucu modüllerine ve hesap yetkilerine bağlıdır. `sql_script` ve `retrain` takvim tariflerini `SDP_JOB_CAPABILITIES` yanıtındaki `kinds` dizisinden denetleyin.
- Yazma veya commit cevabı kaybolduğunda aynı kimlikle yeniden bağlanıp işlemin token’ını `sdp_tx_outcome` ile sorgulayın. `unknown` sonucu belirsiz bırakır.

[ABI, hata ve bellek sözleşmesi](ABI.md) · [Server yetenekleri ve uyumluluk matrisi](COMPATIBILITY.md) · [Sabit wire arşivi](contracts/README.md)

## Kaynaktan derleme ve paketleme

CMake 4.2.2, Visual Studio 2026 C++ (v145), Windows SDK 10.0.26100.0 ve statik OpenSSL 3.5.9 x64 kütüphaneleri gerekir.

```powershell
cmake --preset windows-x64 -DOPENSSL_ROOT_DIR="C:/deps/openssl-3.5.9"
cmake --build --preset windows-x64
ctest --preset windows-x64
cpack --config out/build/windows-x64/CPackConfig.cmake -C Release -B out/packages
```

[Çevrimdışı bağımlılıklar, lisanslar ve kurulu paketle TLS entegrasyon testleri](BUILD.md) · [Server ve diğer SDK’lar](https://github.com/ParadiseIncDevelopers/StarDataParadiseServer) · [Sürüm notları](https://github.com/ParadiseIncDevelopers/StarDataParadiseDLLler/releases)
