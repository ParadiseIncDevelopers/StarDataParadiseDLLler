# StarDataParadiseDLLler C ABI 1

Windows x64, C çağrı sözleşmesi `__cdecl`, doğal x64 alan hizalaması. Kütüphane sürümü `sdp_library_version()` ile ve ABI sürümü `sdp_abi_version()` ile ayrı okunur. Çağıran `sdp_open_options` yapısını sıfırlar, `struct_size = sizeof(sdp_open_options)` ve `abi_version = SDP_ABI_VERSION` yazar.

## Bağlantı

- `address` sayısal IPv4 veya IPv6 adresi. `server_name` sertifikanın SAN alanında doğrulanacak DNS adı (ASCII veya punycode) veya IP adresi. Örnek olarak `127.0.0.1` adresine bağlanırken `localhost` doğrulanabilir.
- `ca_file` UTF-8 bir dosya yoludur. TLS 1.2 ve üstünde sertifika zinciri bu PEM güven paketiyle ve hedef adıyla doğrulanır. Açma işlemi TCP/TLS, hello, auth ve capabilities adımlarını tamamlar.
- `username`, `password` ve diğer girdiler UTF-8 ve uzunlukludur. Çağrı boyunca geçerli kalır. Boyuta sonlandırıcı NUL dahil edilmez. `timeout_ms` 100–120000 ms aralığındadır ve açma ile her gidiş-dönüş için toplam ağ süresini sınırlar.
- `sdp_get_capabilities` bağlantı kurulurken alınan feature JSON nesnesini verir. Hazır sorgular, başlatılmış veri deposu olan ve `sql_crud_v1` yeteneğini ilan eden Server'da çalışır. Veri deposu olmayan Server SQL çağrılarına `SDP_FEATURE_UNSUPPORTED` döndürür.

## Sahiplik, iptal ve iş parçacıkları

`sdp_connection` modüle özel, kopyalanabilir 64 bit bir kimliktir. 0 geçersizdir. Kimlikler modül yüklüyken yeniden kullanılmaz. Bağlantı başına bir ağ işlemi yürütülür. Farklı bağlantılar bağımsızdır. Sonuç buffer'ları boş/sıfırlanmış struct'a yazılır. Bayt sayısı `size` alanındadır. Buffer'ı `sdp_buffer_free` ile serbest bırak, aynı sahiplik kopyasını başka bir thread'den eşzamanlı serbest bırakma.

Başarı veya hata `sdp_status` ile döner. Opsiyonel `sdp_error` çağıran tarafından ayrılır ve çağrı tarafından yazılır. Kaynak konumunda 0 ve correlation ID'de boş dize alanın bulunmadığını gösterir. Statik hata metinleri, SQLSTATE ve makine kodları wire v1 sözleşmesinden gelir.

`sdp_health_async` isteği kuyruğa alır ve tamamlanma callback'ini tam bir kez, kütüphanenin worker thread'inde çağırır. Callback bir sonraki ağ işlemini başlatmaz. Callback içinde `sdp_close` çağrısı `SDP_INVALID_STATE` döndürür. Callback'in error işaretçisi yalnız callback süresince geçerlidir. Callback ve context, `sdp_close` dönene kadar kullanılabilir kalmalı. Kullanıcı callback'i makul sürede dönmeli.

`sdp_cancel` devam eden yerel ağ işleminin iptalini ister. Ağ işlemi iptal işlendiğinde sürüyorsa `SDP_CANCELLED` ile tamamlanır. Tamamlanmış isteğin sonucu korunur. Timeout, iptal veya kopma sonrasında geçerli stream bırakılır ve yeni işlem için tekrar `sdp_open` çağrılır. `sdp_close` bağlantıyı kapatır, worker'ı bekler ve yinelenen çağrıda başarı döndürür. Aynı bağlantıyı başka bir iş parçacığı da kapatıyorsa çağrılar aynı kapanışın tamamlanmasını bekler. Modülü kaldırmadan önce çağıran tüm bağlantıları kapatır, tüm buffer'ları serbest bırakır ve modüle yapılan çağrıları bitirir.

## Hazır sorgular ve transaction

`sdp_prepare` parametresiz SQL için çalışır. `sdp_prepare_typed` SQL ve parametrelerin adlandırılmış tip bildirimlerini iki ayrı `sdp_text` girdisi olarak alır. İkisi de bağlantıya ait opak statement ID'nin baytlarını döndürür. ID metnine C dizgesi sonlandırıcısı dahil değildir. Bildirim örneği `{"id":{"type":"BIGINT"},"price":{"type":"DECIMAL","precision":38,"scale":4}}` şeklindedir. Parametre adı SQL'de `:id` biçiminde kullanılır. Çağıran parametresiz nesneyi sıfır uzunlukla geçebilir.

`sdp_execute`, statement ID ile tipli değer nesnesini alır. Örneğin `{"id":{"t":"i64","v":"9223372036854775807"},"price":{"t":"decimal","unscaled":"123400","scale":4}}` ondalıkları ve tamsayıları kayıpsız iletir. NULL değeri için `{"t":"null"}` kullanılır. Girdiler UTF-8'dir. İsimler ve tipler Server'da bağlanır. Sonuç JSON'u `affected_rows`, varsa `lsn`, sorgu satırları için `result_id` ve `columns` içerir.

`sdp_fetch` opak result ID ve 1–1024 arası batch sınırı ile bir JSON sonuç batch'i döndürür. Her setin ilk batch'i sütun bilgisi taşır. PL/SQL çağrıları `out` setini ve adlandırılmış cursor setlerini döndürebilir. Her PL/SQL setinin ilk batch'inde `set_name` bulunur. `result_set` sıfırdan başlar ve `end_of_set` sonrasında artar. `batch_index` her sette sıfırdan başlar. `end_of_results` yalnız son set bitince true olur ve okuyucuyu serbest bırakır. Her çağrı tek batch verir. Her bağlantıda aynı anda bir açık okuyucu vardır. `sdp_result_cancel` açık okuyucuyu kapatır. `sdp_statement_close` hazır sorgu kimliğini serbest bırakır. `sdp_metadata` mevcut nesne ve sütun bilgilerini JSON olarak döndürür.

`sdp_tx_begin`, `sdp_tx_commit` ve `sdp_tx_rollback` açık işlemi yönetir. `sdp_tx_savepoint`, `sdp_tx_rollback_to` ve `sdp_tx_release` 1–64 ASCII harf, rakam veya alt çizgiden oluşan savepoint adlarını kullanır. Açık işlem içindeki çağrılarda `sdp_execute` için request token boş geçilir. Autocommit yazması ve `sdp_tx_commit` için her yazmaya özel 32 küçük harfli hex karakterden oluşan request token gerekir. SELECT çağrısında token boş geçilir. `sdp_execute` satır sınırı 1–1.000.000 aralığındadır.

Token taşıyan yazma isteği gönderildikten sonra taşıma katmanı bağlantıyı kaybederse SDK `unknown_outcome` alanını işaretler ve otomatik tekrar yapmaz. Yeni bağlantıda `sdp_tx_outcome` ile aynı token sorgulanır. Sonuç `committed` ve `lsn` veya `unknown` olur. Dijital tip bilgisi, sonuçlar için de v1 JSON tip etiketleriyle korunur. Her çıktı `sdp_buffer_free` ile bırakılır.

### PL/SQL bileşik parametreleri

Hazırlanmış PL/SQL çağrılarında `RECORD` ve `COLLECTION` bildirimleri kullanılır. Record, sıralı ve benzersiz adlı `fields` dizisi tutar. Her öğede `name` ve tip etiketli `value` vardır. Collection, tip etiketli `items` dizisi tutar. İç içe bileşik değerlerde tam tamsayı, decimal, NULL, binary ve sayısal timezone bilgisi korunur. İç içe tarih ve saat değerlerinin etiketleri `date` ve `time` olur. Tek bileşik değer 64 KiB, en çok sekiz bileşik katman, katman başına 128 öğe ve toplam 2048 tipli düğüm ile sınırlıdır.

## Oturum yönetimi ve parola

`sdp_session_reset` kimliği korur ve oturuma bağlı açık işlemi geri alır. Hazır sorgular, sonuç okuyucuları ve geçici oturum durumu bırakılır. Reset başarılı döndüğünde bağlantı, aynı kimlik altında tekrar kullanılabilir. Başka bir kullanıcı için yeni bir TLS bağlantısı açılır.

`sdp_session_list` yetkilendirilmiş çağırana `sessions` dizisini içeren JSON döndürür. `sdp_query_cancel` hedef oturum için iptal ister. Request ID 0 sunucudaki aktif isteği seçer. Kabul edilmiş iptal, hedef işlemin tamamlandığı anlamına gelmez. Hedef işlemin son durumunu isteği başlatan çağıran alır. `sdp_session_terminate` hedef oturumu kapatır. Çalışan bir sorguya iptal veya kapatma göndermek için ayrı bir kontrol bağlantısı gerekir.

`sdp_password_set` kullanıcı adı, eski parola, yeni parola ve 32 küçük harfli hex karakterden oluşan request token alır. Yetkili yönetici eski parolayı boş geçebilir. SDK parola alanlarını ağ iletimi sonrasında temizler. Giriş dizilerinin sahipliği çağıranda kalır. Yanıt kaybolursa `unknown_outcome` ve `sdp_tx_outcome` diğer token taşıyan yazmalardaki gibi çalışır.

## PL/SQL debugger, test ve profiler

`sdp_debug_open` ayrı bir kontrol bağlantısından hedef oturum kimliği, rutin adı ve sabit rutin revision’ı ile debug kimliği alır. `sdp_debug_breakpoint` pozitif kaynak satırını ve aynı revision’ı ister. `sdp_debug_status` durum, stack frame ve erişilebilir tipli locals bilgisi döndürür. `sdp_debug_step` için `SDP_STEP_INTO`, `SDP_STEP_OVER` ve `SDP_STEP_OUT` sabitleri kullanılır. `sdp_debug_watch` görünür bir local için ad, tip ve tip etiketli değeri JSON olarak döndürür. `sdp_debug_options` exception-stop ayarını yönetir. `sdp_debug_continue` yürütmeyi sürdürür ve `sdp_debug_close` debug oturumunu kapatır.

`sdp_profile_last` rutini çağıran bağlantının son profil örneklerini döndürür. Süreler mikrosaniye cinsinden duvar saati ölçümleridir. `sdp_routine_test` yalnız özel test konfigürasyonunda hazırlanmış rutin çağrısına tipli parametreleri ve beklenen sonuç setlerini veya exception adını iletir. Test yürütmesi rollback ile kapanır ve karşılaştırma sonucu `passed` alanında döner.

## History

`sdp_history_command` `sdp_history_operation` ile LOG, DIFF, READ, VERIFY, STATUS, RETAIN, PURGE, PIN ve UNPIN çağrılarını seçer. Girdi ve çıktı UTF-8 JSON nesnesidir. Sıfır uzunluk boş nesne anlamına gelir. `target` SQL nitelikli nesne adıdır. `revision`, `from`, `to`, `row_id` ve `quota_bytes` pozitif ondalık dizgelerdir. `offset` ve `limit` işaretsiz tam sayı, `since_utc` ve `until_utc` UTC epoch saniyesi olarak işaretli 64 bit tam sayıdır. Geçmiş kayıtları güncel History ve nesne okuma yetkilerine tabidir.

RETAIN ve PURGE için `revision` açıkça seçilir. PIN ve UNPIN `pin_kind` ve `pin_name` alır. PIN ayrıca `revision` alır. Desteklenen pin türleri `snapshot`, `dataset`, `backup` ve `restore` olur. Elde tutma veya pin değişikliği sırasında yanıt kaybolursa `unknown_outcome` değerlendirilir ve yeni bir bağlantıdan STATUS ile geçerli politika okunabilir. Artık erişilemeyen revision, `SDP_INVALID_STATE` ve `revision_not_recoverable` tanılamasıyla döner.

### RESTORE ve REVERT

`history_restore_v1` yeteneğini ilan eden Server ile `SDP_HISTORY_RESTORE` veya `SDP_HISTORY_REVERT` seçilir. Önizleme `target` ve `revision` ile isteğe bağlı `row_id` ve `field` alır. Yanıt plan kimliğini, mevcut revision’ı, etkilenen nesneyi, bağımlılıkları ve işlem sayılarını içerir. Uygulama isteği yalnız `plan_id` ve `request_token` taşır. İkisi de 32 küçük harfli hex karakterdir.

Onaylanan uygulama `status: committed`, yeni `lsn` ve `replayed` bilgisini döndürür. Yanıt kaybında C API `unknown_outcome` işaretleyebilir. Çağıran aynı kimlikle yeniden bağlanıp `sdp_tx_outcome` ile token sonucunu sorgulayabilir. Aynı plan ve token’ın tekrar gönderilmesi dayanıklı kaydı kullanır. Mevcut revision değişmişse yeni bir önizleme gerekir.

## Paket kullanımı

Paketin SHA-256 değerini kontrol ettikten sonra ZIP'i aç ve kurulum dizinini `CMAKE_PREFIX_PATH` ile ver.

```cmake
find_package(StarDataParadiseDLLler 0.2.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_client PRIVATE StarDataParadise::StarDataParadiseDLLler)
```

`bin/StarDataParadiseDLLler.dll` çalıştırılabilir dosyanın yanında veya uygulamanın güvenilir DLL arama yolunda bulunmalı. Derlenmiş örnek tüketici için `tests/consumer` dizinine bak.
