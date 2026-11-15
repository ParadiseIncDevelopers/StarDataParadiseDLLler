# Native SDK 0.4.0 uyumluluk matrisi

| Server yeteneği | Native ABI 1 çağrıları | Wire v1 |
| --- | --- | --- |
| TLS, kimlik, platform | `sdp_open`, `sdp_get_session_info`, `sdp_get_capabilities`, `sdp_health` | `hello`, `auth`, `capabilities`, `health` |
| SQL ve metadata | `sdp_prepare_typed`, `sdp_execute`, `sdp_fetch`, `sdp_metadata`, statement/reader kapatma | `query.prepare`, `execute`, `fetch`, `metadata`, `statement_close`, `result_cancel` |
| Transaction ve commit sonucunu sorgulama | `sdp_tx_*` | `tx_begin`, `tx_commit`, `tx_rollback`, `tx_savepoint`, `tx_rollback_to`, `tx_release`, `tx_outcome` |
| Oturum denetimi, SQL iptali ve parola | `sdp_session_*`, `sdp_query_cancel`, `sdp_password_set` | `session_reset`, `session_list`, `session_terminate`, `query_cancel`, `password_set` |
| PL/SQL ve araçları | Tipli prepare/execute/fetch, `sdp_routine_test`, `sdp_profile_last`, `sdp_debug_*` | SQL işlem yolu, `routine_test`, `profile_last`, `debug_*` |
| History | `sdp_history_command` | `history_*` |
| Decision | `sdp_decision_command` | `decision_*` |
| İş ve takvim | `sdp_job_command` | `job_*` |
| Dataset ve model | `sdp_dataset_command`, `sdp_model_command` | `dataset_*`, `model_*` |
| Yedek ve yönetim | `sdp_backup_command`, `sdp_admin_command` | `backup_*`, `admin_*` |

`sdp_get_session_info` kimliği doğrulanmış bağlantının `hello` yanıtındaki `session_id`, `product_version` ve `protocol_version` değerlerini döndürür. Ağ isteği göndermez. Yeni bağlantı yeni kimlik alır. Server tarafı sorgu iptalinde ayrı kontrol bağlantısı ve hedef oturum kimliği kullanılır.

`kind: compute`, `kind: backup` ve `kind: maintenance` iş ve takvim çağrılarında mevcut Server seçeneklerini taşır. Backup ve bakım için Server yetki ve modül denetimleri uygulanır. Bakım işinin `action` alanı `integrity`, `statistics` veya `index` değeridir. Çalışan Server’ın feature alanları kurulu ve etkin modülleri gösterir. Sembol bulunması Server yetkisi ya da modül durumu anlamına gelmez.

## Paket ve yaşam döngüsü

Windows x64 Release ZIP’i C ABI 1 DLL ve import library, C/C++ başlıkları, CMake import hedefi, sürümlü wire arşivi ve örnek C++ projesi taşır. Etiketli 0.3.0 DLL’in 45 dış sembolü korunur. 0.4.0’da `sdp_get_session_info` eklenir. Debug ve AddressSanitizer çıktıları ayrı dizinlerdedir ve Release paketi olarak dağıtılmaz. Kullanım için MSVC x64 dinamik çalışma zamanı gerekir. Mimari ve CMake paketi sürümü tüketici derlemesinde seçilir.

C++ sarmalayıcıları çağıran uygulamada başlık üzerinden derlenir. `Connection`, `Statement`, `Transaction` ve `Buffer` birer taşınabilir sahiplik nesnesidir. Statement ve transaction bağlantıdan önce kapatılır. Sonuç `Buffer::view()` yalnız sahibi değişene veya kapanana kadar yaşar. `Buffer::put()` önceki sonucu bırakır ve yeni C ABI çağrısı için boş çıktı işaretçisi döndürür. Connection kapatma ve sahiplik devri callback dışındaki çağıranda yapılır. Eşzamanlı kullanımda ayrı bağlantılar kullanılır. Bağlantı başına tek ağ işlemi vardır. Havuz sunulmaz. Aynı kimliğin yeniden kullanımında başarılı `sdp_session_reset` gereklidir.

SDK yazmaları ya da okumaları otomatik olarak yeniden göndermez. Bağlantı kaybında bağımsız, durum taşımayan `health`, `capabilities` veya `tx_outcome` okumaları çağıranın yeni bağlantısında yapılabilir. Açık okuyucu, debug yürütmesi veya transaction yeni bağlantıya taşınmaz. Şüpheli yazma veya commit için aynı token’ı yeni kimlik doğrulanmış bağlantıda `sdp_tx_outcome` ile sorgula. Geçerli kayıt yalnız `committed` durumunu doğrular. `unknown`, kayıt bulunmadığında veya saklama süresi sona erdiğinde yazmanın gerçekleşmediğini doğrulamaz. C++ transaction sarmalayıcısı commit sonucu belirsizse otomatik rollback veya tekrar gönderim yapmaz.
