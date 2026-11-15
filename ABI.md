# StarDataParadiseDLLler C ABI 1

Windows x64, C çağrı sözleşmesi `__cdecl`, doğal x64 alan hizalaması. Kütüphane sürümü `sdp_library_version()` ile ve ABI sürümü `sdp_abi_version()` ile ayrı okunur. Çağıran `sdp_open_options` yapısını sıfırlar, `struct_size = sizeof(sdp_open_options)` ve `abi_version = SDP_ABI_VERSION` yazar.

## Bağlantı

- `address` sayısal IPv4 veya IPv6 adresi. `server_name` sertifikanın SAN alanında doğrulanacak DNS adı (ASCII veya punycode) veya IP adresi. Örnek olarak `127.0.0.1` adresine bağlanırken `localhost` doğrulanabilir.
- `ca_file` UTF-8 bir dosya yoludur. TLS 1.2 ve üstünde sertifika zinciri bu PEM güven paketiyle ve hedef adıyla doğrulanır. Açma işlemi TCP/TLS, hello, auth ve capabilities adımlarını tamamlar.
- `username`, `password` ve diğer girdiler UTF-8 ve uzunlukludur. Çağrı boyunca geçerli kalır. Boyuta sonlandırıcı NUL dahil edilmez. `timeout_ms` 100–120000 ms aralığındadır ve açma ile her gidiş-dönüş için toplam ağ süresini sınırlar.
- `sdp_get_capabilities` bağlantı kurulurken alınan feature JSON nesnesini verir. `sdp_prepare` doğrudan Server'a hazır sorgu isteği yollar ve başarılıysa opaque statement ID döndürür. İlk Server sürümünde bu çağrı `SDP_FEATURE_UNSUPPORTED` döndürür.

## Sahiplik, iptal ve iş parçacıkları

`sdp_connection` modüle özel, kopyalanabilir 64 bit bir kimliktir. 0 geçersizdir. Kimlikler modül yüklüyken yeniden kullanılmaz. Bağlantı başına bir health/prepare işlemi yürütülür. Farklı bağlantılar bağımsızdır. Sonuç buffer'ları boş/sıfırlanmış struct'a yazılır. Bayt sayısı `size` alanındadır. Buffer'ı `sdp_buffer_free` ile serbest bırak, aynı sahiplik kopyasını başka bir thread'den eşzamanlı serbest bırakma.

Başarı veya hata `sdp_status` ile döner. Opsiyonel `sdp_error` çağıran tarafından ayrılır ve çağrı tarafından yazılır. Kaynak konumunda 0 ve correlation ID'de boş dize alanın bulunmadığını gösterir. Statik hata metinleri, SQLSTATE ve makine kodları wire v1 sözleşmesinden gelir.

`sdp_health_async` isteği kuyruğa alır ve tamamlanma callback'ini tam bir kez, kütüphanenin worker thread'inde çağırır. Callback bir sonraki ağ işlemini başlatmaz. Callback içinde `sdp_close` çağrısı `SDP_INVALID_STATE` döndürür. Callback'in error işaretçisi yalnız callback süresince geçerlidir. Callback ve context, `sdp_close` dönene kadar kullanılabilir kalmalı. Kullanıcı callback'i makul sürede dönmeli.

`sdp_cancel` devam eden yerel ağ işleminin iptalini ister. Ağ işlemi iptal işlendiğinde sürüyorsa `SDP_CANCELLED` ile tamamlanır. Tamamlanmış isteğin sonucu korunur. Timeout, iptal veya kopma sonrasında geçerli stream bırakılır ve yeni işlem için tekrar `sdp_open` çağrılır. `sdp_close` bağlantıyı kapatır, worker'ı bekler ve yinelenen çağrıda başarı döndürür. Aynı bağlantıyı başka bir iş parçacığı da kapatıyorsa çağrılar aynı kapanışın tamamlanmasını bekler. Modülü kaldırmadan önce çağıran tüm bağlantıları kapatır, tüm buffer'ları serbest bırakır ve modüle yapılan çağrıları bitirir.

## Paket kullanımı

Paketin SHA-256 değerini kontrol ettikten sonra ZIP'i aç ve kurulum dizinini `CMAKE_PREFIX_PATH` ile ver.

```cmake
find_package(StarDataParadiseDLLler 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_client PRIVATE StarDataParadise::StarDataParadiseDLLler)
```

`bin/StarDataParadiseDLLler.dll` çalıştırılabilir dosyanın yanında veya uygulamanın güvenilir DLL arama yolunda bulunmalı. Derlenmiş örnek tüketici için `tests/consumer` dizinine bak.
