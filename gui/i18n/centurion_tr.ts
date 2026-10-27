<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="tr_TR">
    <context>
        <name>CoolingPage</name>
        <message>
            <location filename="../src/coolingpage.cpp" line="46" />
            <source>Other programs were using the CPU during the test, so this run cannot be compared with the baseline. Close them and run the test again.</source>
            <translation>Test sırasında başka programlar işlemciyi kullanıyordu; bu çalıştırma referansla karşılaştırılamaz. Onları kapatıp testi yeniden çalıştırın.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="48" />
            <source>The laptop started this test %1 °C warmer than it did at the baseline (a warmer room, or it had not cooled down). Let it rest and run the test again.</source>
            <translation>Dizüstü bu teste referanstakinden %1 °C daha sıcak başladı (daha sıcak bir oda ya da yeterince soğumamış olabilir). Dinlendirip testi yeniden çalıştırın.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="51" />
            <source>Cooling is as good as the baseline: the same load runs at about the same temperature.</source>
            <translation>Soğutma referans kadar iyi: aynı yük yaklaşık aynı sıcaklıkta çalışıyor.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="53" />
            <source>Cooling is better than the baseline (%1 °C). If you have just cleaned the laptop, press "Use the latest run as baseline".</source>
            <translation>Soğutma referanstan daha iyi (%1 °C). Dizüstüyü yeni temizlediyseniz "Son çalıştırmayı referans yap" düğmesine basın.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="56" />
            <source>Cooling is slightly weaker than the baseline. Repeat the test in a few weeks; if it keeps drifting, clean the vents and fans.</source>
            <translation>Soğutma referanstan biraz zayıf. Testi birkaç hafta sonra tekrarlayın; düşmeye devam ederse havalandırma ızgaralarını ve fanları temizleyin.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="58" />
            <source>Cooling is clearly weaker than the baseline. Dust in the heat sink and fans, or dried-out thermal paste, are the usual causes: clean the vents and fans first, and re-paste only if the difference stays.</source>
            <translation>Soğutma referanstan belirgin şekilde zayıf. Soğutucudaki ve fanlardaki toz ya da kurumuş termal macun olağan nedenlerdir: önce ızgaraları ve fanları temizleyin, fark sürerse macunu yenileyin.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="62" />
            <source>%1 °C hotter at the same load</source>
            <translation>Aynı yükte %1 °C daha sıcak</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="63" />
            <source>clock %1 % lower</source>
            <translation>saat hızı %1 % düşük</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="64" />
            <source>fans %1 % faster</source>
            <translation>fanlar %1 % daha hızlı</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="66" />
            <source>(If you changed the fan curve since the baseline, ignore the fan figure.)</source>
            <translation>(Referanstan beri fan eğrisini değiştirdiyseniz fan değerini yok sayın.)</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="69" />
            <source>The firmware holds the CPU at its %1 °C temperature target, so the temperature cannot rise further: the clock is what shows weaker cooling.</source>
            <translation>Donanım yazılımı işlemciyi %1 °C sıcaklık hedefinde tutuyor, bu yüzden sıcaklık daha fazla yükselemez: zayıf soğutmayı saat hızı gösterir.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="79" />
            <source>Puts a fixed load on every CPU thread for a few minutes and records the temperature, fan speed and clock. The first clean run becomes the baseline; later runs are compared with it, so dust in the fans or dried-out thermal paste show up as a hotter die, louder fans or lower clocks. Results are only compared under the same firmware settings (below).</source>
            <translation>Birkaç dakika boyunca her işlemci iş parçacığına sabit bir yük bindirir; sıcaklığı, fan hızını ve saat hızını kaydeder. İlk temiz çalıştırma referans olur; sonrakiler onunla karşılaştırılır, böylece fanlardaki toz ya da kurumuş termal macun daha sıcak çip, daha gürültülü fanlar veya daha düşük saat hızı olarak görünür. Sonuçlar yalnızca aynı donanım yazılımı ayarlarında (aşağıda) karşılaştırılır.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="88" />
            <source>Run cooling test</source>
            <translation>Soğutma testini çalıştır</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="89" />
            <source>Loads every CPU thread for the chosen time (after a short cool-down). The fans will run loud.</source>
            <translation>Seçilen süre boyunca (kısa bir soğumadan sonra) tüm işlemci iş parçacıklarını yükler. Fanlar gürültülü çalışacak.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="90" />
            <source>Cancel</source>
            <translation>İptal</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="93" />
            <source>2 minutes (short)</source>
            <translation>2 dakika (kısa)</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="94" />
            <source>4 minutes (recommended)</source>
            <translation>4 dakika (önerilen)</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="95" />
            <source>8 minutes (thorough)</source>
            <translation>8 dakika (kapsamlı)</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="97" />
            <source>Use the same length every time you compare.</source>
            <translation>Karşılaştırırken her seferinde aynı süreyi kullanın.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="119" />
            <source>Date</source>
            <translation>Tarih</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="119" />
            <source>CPU temperature</source>
            <translation>İşlemci sıcaklığı</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="119" />
            <source>Fan</source>
            <translation>Fan</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="119" />
            <source>Clock</source>
            <translation>Saat hızı</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="119" />
            <source>Started at</source>
            <translation>Başlangıç sıcaklığı</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="119" />
            <source>Compared with the baseline</source>
            <translation>Referansla karşılaştırma</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="127" />
            <source>Use the latest run as baseline</source>
            <translation>Son çalıştırmayı referans yap</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="128" />
            <source>Make the newest clean run the reference for these firmware settings, e.g. after cleaning the fans.</source>
            <translation>En yeni temiz çalıştırmayı bu donanım yazılımı ayarları için referans yapar; örneğin fanları temizledikten sonra.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="129" />
            <source>Forget results for these settings</source>
            <translation>Bu ayarların sonuçlarını sil</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="134" />
            <source>For a fair comparison: plug in the charger, leave the fans on Auto, close other programs, use the same room and the same test length. A changed power limit or temperature target starts a new baseline, since it changes how much heat the load makes.</source>
            <translation>Adil bir karşılaştırma için: şarj aletini takın, fanları Otomatik bırakın, diğer programları kapatın, aynı ortamı ve aynı test süresini kullanın. Değişen bir güç sınırı ya da sıcaklık hedefi yeni bir referans başlatır, çünkü yükün ürettiği ısıyı değiştirir.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="141" />
            <location filename="../src/coolingpage.cpp" line="146" />
            <source>Cooling</source>
            <translation>Soğutma</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="146" />
            <source>Forget every cooling result stored for the current firmware settings?</source>
            <translation>Geçerli donanım yazılımı ayarları için kayıtlı tüm soğutma sonuçları silinsin mi?</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="171" />
            <location filename="../src/coolingpage.cpp" line="265" />
            <source>centurion-cooltest not found</source>
            <translation>centurion-cooltest bulunamadı</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="187" />
            <source>Could not read the cooling status: %1</source>
            <translation>Soğutma durumu okunamadı: %1</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="187" />
            <source>centurion-cooltest failed</source>
            <translation>centurion-cooltest başarısız oldu</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="195" />
            <source>on AC</source>
            <translation>şebeke gücünde</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="195" />
            <source>on battery</source>
            <translation>pilde</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="195" />
            <source>power source unknown</source>
            <translation>güç kaynağı bilinmiyor</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="196" />
            <source>&lt;b&gt;Compared under:&lt;/b&gt; power profile %1 · %2 · PL1 / PL2 / PL3 %3 / %4 / %5 W</source>
            <translation>&lt;b&gt;Karşılaştırma koşulları:&lt;/b&gt; güç profili %1 · %2 · PL1 / PL2 / PL3 %3 / %4 / %5 W</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="199" />
            <source> · temperature target %1 °C</source>
            <translation> · sıcaklık hedefi %1 °C</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="200" />
            <source> · no firmware temperature target (assuming %1 °C)</source>
            <translation> · donanım yazılımı sıcaklık hedefi yok (%1 °C varsayılıyor)</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="205" />
            <source>&lt;span style='color:%1'&gt;No CPU temperature sensor found (k10temp / coretemp), so the test cannot run.&lt;/span&gt;</source>
            <translation>&lt;span style='color:%1'&gt;İşlemci sıcaklık sensörü bulunamadı (k10temp / coretemp), bu yüzden test çalışamaz.&lt;/span&gt;</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="214" />
            <location filename="../src/coolingpage.cpp" line="217" />
            <location filename="../src/coolingpage.cpp" line="223" />
            <source>%1 °C</source>
            <translation>%1 °C</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="215" />
            <source>%1 rpm</source>
            <translation>%1 dev/dk</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="216" />
            <location filename="../src/coolingpage.cpp" line="294" />
            <source>%1 GHz</source>
            <translation>%1 GHz</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="220" />
            <source>baseline</source>
            <translation>referans</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="221" />
            <source>other programs were busy: not compared</source>
            <translation>diğer programlar meşguldü: karşılaştırılmadı</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="234" />
            <source>This is the baseline for these firmware settings. Run the test again in a few weeks, and after cleaning, to compare.</source>
            <translation>Bu, bu donanım yazılımı ayarları için referanstır. Karşılaştırmak için testi birkaç hafta sonra ve temizlikten sonra yeniden çalıştırın.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="271" />
            <source>The test was stopped.</source>
            <translation>Test durduruldu.</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="275" />
            <source>Cooling down before the test…</source>
            <translation>Testten önce soğuyor…</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="291" />
            <source>%1 / %2 s</source>
            <translation>%1 / %2 sn</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="292" />
            <source>CPU %1 °C</source>
            <translation>İşlemci %1 °C</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="293" />
            <source>fan %1 rpm</source>
            <translation>fan %1 dev/dk</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="295" />
            <source>Testing: %1</source>
            <translation>Test ediliyor: %1</translation>
        </message>
        <message>
            <location filename="../src/coolingpage.cpp" line="319" />
            <source>The test failed.</source>
            <translation>Test başarısız oldu.</translation>
        </message>
    </context>
    <context>
        <name>FirstRunWizard</name>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="66" />
            <source>Your machine</source>
            <translation>Makineniz</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="66" />
            <source>What Centurion found. Tabs appear only for what is here.</source>
            <translation>Centurion'un bulduğu donanım. Sekmeler yalnızca burada olanlar için görünür.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="69" />
            <source>Unknown model</source>
            <translation>Bilinmeyen model</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="70" />
            <source>Not a supported machine: %1</source>
            <translation>Desteklenen bir makine değil: %1</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="72" />
            <source>Supported family, but this model was not tested yet — reports are welcome (Home → Device info…).</source>
            <translation>Desteklenen bir aile, ancak bu model henüz test edilmedi — raporlarınızı bekliyoruz (Ana sayfa → Aygıt bilgisi…).</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="74" />
            <source>Supported model.</source>
            <translation>Desteklenen model.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="78" />
            <source>lenovo-wmi-gamezone (power profiles, fans)</source>
            <translation>lenovo-wmi-gamezone (güç profilleri, fanlar)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="79" />
            <source>lenovo-wmi-other (firmware attributes)</source>
            <translation>lenovo-wmi-other (donanım yazılımı öznitelikleri)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="80" />
            <source>ideapad_laptop (battery, device toggles)</source>
            <translation>ideapad_laptop (pil, aygıt anahtarları)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="81" />
            <source>Spectrum per-key keyboard</source>
            <translation>Spectrum tuş başına aydınlatmalı klavye</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="82" />
            <source>4-zone RGB keyboard</source>
            <translation>4 bölgeli RGB klavye</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="83" />
            <source>NVIDIA GPU</source>
            <translation>NVIDIA GPU</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="84" />
            <source>AMD GPU (amdgpu)</source>
            <translation>AMD GPU (amdgpu)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="99" />
            <source>Boot services</source>
            <translation>Önyükleme hizmetleri</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="100" />
            <source>Optional. Each one does nothing until you set its default or preset in the app.</source>
            <translation>İsteğe bağlı. Her biri, uygulamada varsayılanını ya da ön ayarını belirleyene kadar hiçbir şey yapmaz.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="102" />
            <source>records clean shutdowns; pauses the others after a crash</source>
            <translation>temiz kapanışları kaydeder; çökme sonrası diğerlerini duraklatır</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="103" />
            <source>★ default GPU curve at boot</source>
            <translation>★ açılışta varsayılan GPU eğrisi</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="104" />
            <source>⏻ Optimizations boot preset</source>
            <translation>⏻ Optimizasyonlar açılış ön ayarı</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="105" />
            <source>Intel undervolt at boot / resume</source>
            <translation>Açılışta / uykudan dönüşte Intel undervolt</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="107" />
            <source>network guard (Health → Network)</source>
            <translation>ağ koruması (Sağlık → Ağ)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="113" />
            <source>Neither systemd nor OpenRC was found: see README → Boot services.</source>
            <translation>Ne systemd ne de OpenRC bulundu: README → Boot services bölümüne bakın.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="138" />
            <source>Enabling a service needs root. Tick what you want, then run the command below in a terminal (%1).</source>
            <translation>Bir hizmeti etkinleştirmek için root gerekir. İstediklerinizi işaretleyin, sonra aşağıdaki komutu bir terminalde çalıştırın (%1).</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="146" />
            <source>  (already enabled)</source>
            <translation>  (zaten etkin)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="147" />
            <source>  (recommended)</source>
            <translation>  (önerilen)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="169" />
            <source>Copy command</source>
            <translation>Komutu kopyala</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="180" />
            <source>First backup</source>
            <translation>İlk yedek</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="181" />
            <source>A copy of everything Centurion stores, before you change anything. No root needed.</source>
            <translation>Bir şeyi değiştirmeden önce Centurion'un sakladığı her şeyin bir kopyası. Root gerekmez.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="182" />
            <source>Saved to the folder set in Health → Backup (default ~/Backups/centurion). Automatic backups can be turned on there too.</source>
            <translation>Sağlık → Yedek bölümünde belirlenen klasöre kaydedilir (varsayılan ~/Backups/centurion). Otomatik yedekler de orada açılabilir.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="184" />
            <source>Back up the configuration now</source>
            <translation>Yapılandırmayı şimdi yedekle</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="189" />
            <source>Saving…</source>
            <translation>Kaydediliyor…</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="206" />
            <source>First scene</source>
            <translation>İlk sahne</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="207" />
            <source>A scene is the whole machine under one name: a restore point you can go back to in one click.</source>
            <translation>Sahne, tüm makinenin tek bir ad altındaki hâlidir: tek tıkla dönebileceğiniz bir geri yükleme noktası.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="208" />
            <source>Capture current as '%1'</source>
            <translation>Mevcut durumu '%1' olarak yakala</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="210" />
            <source>Switch scenes automatically with the power source</source>
            <translation>Sahneleri güç kaynağıyla otomatik değiştir</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="215" />
            <source>(leave as is)</source>
            <translation>(olduğu gibi bırak)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="228" />
            <source>A scene named '%1' exists already.</source>
            <translation>'%1' adlı bir sahne zaten var.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="242" />
            <source>Could not save the scene: %1</source>
            <translation>Sahne kaydedilemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="247" />
            <source>Saved '%1': power profile %2, %3 firmware value(s).%4 Fan curve and fan boost: Scenes tab → Capture current.</source>
            <translation>'%1' kaydedildi: güç profili %2, %3 donanım yazılımı değeri.%4 Fan eğrisi ve fan artırımı: Sahneler sekmesi → Mevcut durumu yakala.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="248" />
            <source>unchanged</source>
            <translation>değişmedi</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="250" />
            <source> GPU cTGP / boost were not read yet (open the Firmware tab once).</source>
            <translation> GPU cTGP / boost henüz okunmadı (Donanım yazılımı sekmesini bir kez açın).</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="263" />
            <source>On AC power</source>
            <translation>Şebeke gücünde</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="264" />
            <source>On battery</source>
            <translation>Pilde</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="280" />
            <source>Extras</source>
            <translation>Ekstralar</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="280" />
            <source>All optional; each can be changed later.</source>
            <translation>Hepsi isteğe bağlı; sonradan değiştirilebilir.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="286" />
            <source>Shortcut for '%1'</source>
            <translation>'%1' için kısayol</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="288" />
            <source>Every scene can have one: Scenes tab → Shortcut. It is registered with the desktop (global-shortcuts portal), which may ask you to confirm it.</source>
            <translation>Her sahnenin bir kısayolu olabilir: Sahneler sekmesi → Kısayol. Masaüstüne (global-shortcuts portalı) kaydedilir; masaüstü onayınızı isteyebilir.</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="292" />
            <source>Show the active scene and power profile in games (MangoHud)</source>
            <translation>Etkin sahneyi ve güç profilini oyunlarda göster (MangoHud)</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="297" />
            <source>Scripts, KDE Connect, Home Assistant or a Stream Deck can switch scenes and power profiles and read the sensors over D-Bus: &lt;code&gt;com.centurion.Centurion&lt;/code&gt; (README → D-Bus API).</source>
            <translation>Betikler, KDE Connect, Home Assistant ya da bir Stream Deck, D-Bus üzerinden sahneleri ve güç profillerini değiştirebilir, sensörleri okuyabilir: &lt;code&gt;com.centurion.Centurion&lt;/code&gt; (README → D-Bus API).</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="299" />
            <source>Next: the &lt;a href='%1'&gt;Handbook&lt;/a&gt;, task by task. This wizard again: Home → Setup wizard…</source>
            <translation>Sıradaki: görev görev &lt;a href='%1'&gt;El Kitabı&lt;/a&gt;. Bu sihirbaz tekrar: Ana sayfa → Kurulum sihirbazı…</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="317" />
            <source>Centurion setup</source>
            <translation>Centurion kurulumu</translation>
        </message>
        <message>
            <location filename="../src/firstrunwizard.cpp" line="320" />
            <source>Skip setup</source>
            <translation>Kurulumu atla</translation>
        </message>
    </context>
    <context>
        <name>HealthTab</name>
        <message>
            <location filename="../src/healthtab.cpp" line="71" />
            <source>Monitor</source>
            <translation>İzleme</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="72" />
            <source>dGPU power</source>
            <translation>dGPU gücü</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="77" />
            <source>Network</source>
            <translation>Ağ</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="83" />
            <source>Curve stability</source>
            <translation>Eğri kararlılığı</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="84" />
            <source>Sleep</source>
            <translation>Uyku</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="85" />
            <source>Battery use</source>
            <translation>Pil kullanımı</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="86" />
            <source>Cooling</source>
            <translation>Soğutma</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="88" />
            <source>Kernel log</source>
            <translation>Çekirdek günlüğü</translation>
        </message>
        <message>
            <location filename="../src/healthtab.cpp" line="92" />
            <source>Backup</source>
            <translation>Yedek</translation>
        </message>
    </context>
    <context>
        <name>HomeTab</name>
        <message>
            <location filename="../src/hometab.cpp" line="93" />
            <source>Power Saver</source>
            <translation>Güç Tasarrufu</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="93" />
            <source>Quiet</source>
            <translation>Sessiz</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="94" />
            <source>Cool</source>
            <translation>Serin</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="94" />
            <source>Balanced</source>
            <translation>Dengeli</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="95" />
            <source>Balanced Performance</source>
            <translation>Dengeli Performans</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="96" />
            <source>Performance</source>
            <translation>Performans</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="97" />
            <source>Extreme</source>
            <translation>Aşırı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="97" />
            <source>Custom</source>
            <translation>Özel</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="100" />
            <source>Lowest power draw, longest battery life.</source>
            <translation>En düşük güç tüketimi, en uzun pil ömrü.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="101" />
            <source>Keeps fan noise as low as possible.</source>
            <translation>Fan gürültüsünü olabildiğince düşük tutar.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="102" />
            <source>Prioritises low surface and internal temperatures.</source>
            <translation>Düşük yüzey ve iç sıcaklıklara öncelik verir.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="103" />
            <source>Balanced mix of power, noise and performance.</source>
            <translation>Güç, gürültü ve performansın dengeli karışımı.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="104" />
            <source>Leans towards performance.</source>
            <translation>Performansa yatkındır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="105" />
            <source>Maximum performance; fans run louder.</source>
            <translation>Azami performans; fanlar daha gürültülü çalışır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="106" />
            <source>Unrestricted BIOS limits, mains power only.</source>
            <translation>Kısıtsız BIOS sınırları, yalnızca şebeke gücü.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="107" />
            <source>Hands the PPT and fan limits to you. Required by the Firmware Attributes tab.</source>
            <translation>PPT ve fan sınırlarını size bırakır. Donanım Yazılımı Öznitelikleri sekmesi için gereklidir.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="181" />
            <source>unknown CPU</source>
            <translation>bilinmeyen işlemci</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="182" />
            <source>driver: %1</source>
            <translation>sürücü: %1</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="191" />
            <source>Colour theme. Applied by restarting the app (a few seconds).</source>
            <translation>Renk teması. Uygulamayı yeniden başlatınca uygulanır (birkaç saniye).</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="197" />
            <source>Could not save the theme choice.</source>
            <translation>Tema seçimi kaydedilemedi.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="198" />
            <location filename="../src/hometab.cpp" line="202" />
            <source>Theme</source>
            <translation>Tema</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="198" />
            <source>Restart Centurion now to apply %1?</source>
            <translation>%1 temasını uygulamak için Centurion şimdi yeniden başlatılsın mı?</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="200" />
            <source>Theme applies at the next start.</source>
            <translation>Tema bir sonraki başlatmada uygulanır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="208" />
            <source>Interface language. Applied by restarting the app (a few seconds).</source>
            <translation>Arayüz dili. Uygulamayı yeniden başlatınca uygulanır (birkaç saniye).</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="209" />
            <location filename="../src/hometab.cpp" line="825" />
            <source>Auto</source>
            <translation>Otomatik</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="214" />
            <source>Could not save the language choice.</source>
            <translation>Dil seçimi kaydedilemedi.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="215" />
            <location filename="../src/hometab.cpp" line="218" />
            <source>Language</source>
            <translation>Dil</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="215" />
            <source>Restart Centurion now to change the language?</source>
            <translation>Dili değiştirmek için Centurion şimdi yeniden başlatılsın mı?</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="216" />
            <source>The language changes at the next start.</source>
            <translation>Dil bir sonraki başlatmada değişir.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="233" />
            <source>Resume boot presets</source>
            <translation>Açılış ön ayarlarını sürdür</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="236" />
            <source>Resume login scene</source>
            <translation>Oturum açma sahnesini sürdür</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="248" />
            <source>Could not resume: %1</source>
            <translation>Sürdürülemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="249" />
            <source>Boot presets resume from the next boot.</source>
            <translation>Açılış ön ayarları bir sonraki açılıştan itibaren sürer.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="260" />
            <source>&lt;b&gt;⏸ Scenes paused&lt;/b&gt; — no automatic scene changes (power source, login, game) until resumed.</source>
            <translation>&lt;b&gt;⏸ Sahneler duraklatıldı&lt;/b&gt; — sürdürülene kadar otomatik sahne değişikliği yok (güç kaynağı, oturum açma, oyun).</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="264" />
            <source>▶ Resume scenes</source>
            <translation>▶ Sahneleri sürdür</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="270" />
            <source>Power Profile</source>
            <translation>Güç Profili</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="302" />
            <source>Per-component limits and undervolt curves live in the other tabs. Firmware Attributes only accepts writes while the profile is Custom.</source>
            <translation>Bileşen başına sınırlar ve undervolt eğrileri diğer sekmelerde. Donanım Yazılımı Öznitelikleri yalnızca profil Özel iken yazma kabul eder.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="325" />
            <source>Hardware</source>
            <translation>Donanım</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="346" />
            <source>System</source>
            <translation>Sistem</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="354" />
            <source>Device info…</source>
            <translation>Aygıt bilgisi…</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="359" />
            <source>Setup wizard…</source>
            <translation>Kurulum sihirbazı…</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="366" />
            <source>Kernel</source>
            <translation>Çekirdek</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="368" />
            <location filename="../src/hometab.cpp" line="412" />
            <source>GPU</source>
            <translation>GPU</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="371" />
            <source>Memory</source>
            <translation>Bellek</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="379" />
            <source>Timings…</source>
            <translation>Zamanlamalar…</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="386" />
            <location filename="../src/hometab.cpp" line="686" />
            <location filename="../src/hometab.cpp" line="1524" />
            <location filename="../src/hometab.cpp" line="1540" />
            <source>GPU mode</source>
            <translation>GPU modu</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="388" />
            <source>EC Firmware</source>
            <translation>EC Yazılımı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="404" />
            <source>Live</source>
            <translation>Canlı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="412" />
            <source>CPU Temp</source>
            <translation>İşlemci Sıc.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="412" />
            <source>iGPU</source>
            <translation>iGPU</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="413" />
            <location filename="../src/hometab.cpp" line="900" />
            <source>Fans</source>
            <translation>Fanlar</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="413" />
            <source>Storage</source>
            <translation>Depolama</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="413" />
            <source>Power</source>
            <translation>Güç</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="414" />
            <source>CPU Power</source>
            <translation>İşlemci Gücü</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="414" />
            <source>USB-C in</source>
            <translation>USB-C girişi</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="415" />
            <source>Battery</source>
            <translation>Pil</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="440" />
            <source>Fans (EC)</source>
            <translation>Fanlar (EC)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="442" />
            <source>Charger</source>
            <translation>Şarj aleti</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="451" />
            <source>No live sensors found (hwmon, battery).</source>
            <translation>Canlı sensör bulunamadı (hwmon, pil).</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="510" />
            <source>off (not on the PCI bus)</source>
            <translation>kapalı (PCI veriyolunda değil)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="512" />
            <source>asleep (runtime suspended, ~0 W)</source>
            <translation>uykuda (çalışma zamanı askıda, ~0 W)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="514" />
            <source>%1°C  ·  awake</source>
            <translation>%1°C  ·  uyanık</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="514" />
            <source>awake</source>
            <translation>uyanık</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="572" />
            <source>%1 %2 RPM</source>
            <translation>%1 %2 dev/dk</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="580" />
            <source>&lt;span style='color:%1'&gt;low-wattage charger — the firmware limits CPU/GPU power&lt;/span&gt;</source>
            <translation>&lt;span style='color:%1'&gt;düşük watt'lı şarj aleti — donanım yazılımı işlemci/GPU gücünü sınırlıyor&lt;/span&gt;</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="645" />
            <source>Device</source>
            <translation>Aygıt</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="663" />
            <source>Battery charge</source>
            <translation>Pil şarjı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="670" />
            <location filename="../src/hometab.cpp" line="1504" />
            <source>Hybrid (iGPU + NVIDIA)</source>
            <translation>Hibrit (iGPU + NVIDIA)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="671" />
            <location filename="../src/hometab.cpp" line="1505" />
            <source>dGPU only (MUX → NVIDIA)</source>
            <translation>Yalnızca dGPU (MUX → NVIDIA)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="691" />
            <location filename="../src/hometab.cpp" line="744" />
            <location filename="../src/hometab.cpp" line="749" />
            <source>iGPU mode</source>
            <translation>iGPU modu</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="693" />
            <source>Default</source>
            <translation>Varsayılan</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="694" />
            <source>iGPU only (NVIDIA cut off)</source>
            <translation>Yalnızca iGPU (NVIDIA kapalı)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="695" />
            <source>Auto (iGPU only on battery)</source>
            <translation>Otomatik (pilde yalnızca iGPU)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="699" />
            <location filename="../src/hometab.cpp" line="765" />
            <source>Panel Over Drive</source>
            <translation>Panel Over Drive</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="702" />
            <source>Bring NVIDIA back</source>
            <translation>NVIDIA'yı geri getir</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="744" />
            <source>

Apply anyway? (asks for the administrator password)</source>
            <translation>

Yine de uygulansın mı? (yönetici parolası istenir)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="759" />
            <source>NVIDIA driver loaded.</source>
            <translation>NVIDIA sürücüsü yüklendi.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="759" />
            <source>%1; driver loaded.</source>
            <translation>%1; sürücü yüklendi.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="771" />
            <source>Fn lock</source>
            <translation>Fn kilidi</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="771" />
            <source>Camera</source>
            <translation>Kamera</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="771" />
            <source>USB charging when off</source>
            <translation>Kapalıyken USB şarjı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="786" />
            <source>Switches</source>
            <translation>Anahtarlar</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="800" />
            <source>Off</source>
            <translation>Kapalı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="801" />
            <source>On</source>
            <translation>Açık</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="802" />
            <source>Low</source>
            <translation>Düşük</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="802" />
            <source>High</source>
            <translation>Yüksek</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="803" />
            <source>Level %1</source>
            <translation>Seviye %1</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="806" />
            <source>Keyboard light</source>
            <translation>Klavye ışığı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="819" />
            <source>RPM</source>
            <translation>dev/dk</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="828" />
            <source>Set</source>
            <translation>Ayarla</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="833" />
            <source>Enter an RPM value first</source>
            <translation>Önce bir dev/dk değeri girin</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="839" />
            <source>Max</source>
            <translation>Azami</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="864" />
            <location filename="../src/hometab.cpp" line="1197" />
            <source>Fan %1</source>
            <translation>Fan %1</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="893" />
            <source>Turbo fan (EC full speed)</source>
            <translation>Turbo fan (EC tam hız)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="927" />
            <location filename="../src/hometab.cpp" line="1170" />
            <source>Disable max fans</source>
            <translation>Azami fanları kapat</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="934" />
            <source>Max all fans</source>
            <translation>Tüm fanlar azami</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="939" />
            <source>Fan curve…</source>
            <translation>Fan eğrisi…</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="973" />
            <source>Firmware</source>
            <translation>Donanım yazılımı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="981" />
            <source>Boot on AC</source>
            <translation>Şebekede aç</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="981" />
            <source>Power on automatically when the charger is plugged in (lid open).</source>
            <translation>Şarj aleti takılınca (kapak açıkken) otomatik açılır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="982" />
            <source>Boot on USB-PD</source>
            <translation>USB-PD ile aç</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="982" />
            <source>Power on automatically from a USB-C PD charger.</source>
            <translation>USB-C PD şarj aletinden otomatik açılır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="983" />
            <source>Custom in Fn+Q</source>
            <translation>Fn+Q'da Özel</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="983" />
            <source>Include the Custom profile in the Fn+Q cycle.</source>
            <translation>Özel profili Fn+Q döngüsüne ekler.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="984" />
            <source>Flip to start</source>
            <translation>Kapağı açınca başlat</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="984" />
            <source>Power on when the lid is opened.</source>
            <translation>Kapak açılınca güç verir.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="985" />
            <source>Super key lock</source>
            <translation>Süper tuş kilidi</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="985" />
            <source>Disable the Super (Windows) key of the built-in keyboard — e.g. while gaming.</source>
            <translation>Dahili klavyenin Süper (Windows) tuşunu devre dışı bırakır — örneğin oyun sırasında.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="986" />
            <source>Touchpad lock</source>
            <translation>Dokunmatik yüzey kilidi</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="986" />
            <source>Disable the touchpad in the embedded controller.</source>
            <translation>Gömülü denetleyicide dokunmatik yüzeyi devre dışı bırakır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="987" />
            <source>Fan cooling</source>
            <translation>Fan soğutma</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="987" />
            <source>Firmware fan-cooling run (GameZone SetFanCooling).
The firmware may end it by itself; the switch then reads off again.</source>
            <translation>Donanım yazılımının fan soğutma çalışması (GameZone SetFanCooling).
Donanım yazılımı bunu kendiliğinden bitirebilir; anahtar o zaman yine kapalı görünür.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="989" />
            <source>USB charging on battery</source>
            <translation>Pilde USB şarjı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="990" />
            <source>Keep the always-on USB port powered while the laptop is off or asleep on battery too
(needs "USB charging when off").</source>
            <translation>Dizüstü pilde kapalı ya da uykudayken de her zaman açık USB bağlantı noktasına güç verir
("Kapalıyken USB şarjı" gerekir).</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1004" />
            <location filename="../src/hometab.cpp" line="1301" />
            <source>%1 failed: %2</source>
            <translation>%1 başarısız: %2</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1005" />
            <source>%1 → %2</source>
            <translation>%1 → %2</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1005" />
            <source>on</source>
            <translation>açık</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1005" />
            <source>off</source>
            <translation>kapalı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1110" />
            <source>&lt;span style='color:%1'&gt;The fans run at maximum with no target set: the EC's Full Speed mode is on (it survives reboots, e.g. switched on in Windows). This kernel has no interface to turn it off — lenovo_wmi_other lacks pwm1_enable and legion_laptop is not loaded. Turn it off in Lenovo Vantage / Legion Space, or load LenovoLegionLinux's legion_laptop module; an EC reset (power off, hold the power button ~30 s) also clears it.&lt;/span&gt;</source>
            <translation>&lt;span style='color:%1'&gt;Fanlar hedef belirlenmeden azami hızda çalışıyor: EC'nin Tam Hız modu açık (yeniden başlatmalara dayanır; örneğin Windows'ta açılmış olabilir). Bu çekirdekte onu kapatacak bir arayüz yok — lenovo_wmi_other'da pwm1_enable eksik ve legion_laptop yüklü değil. Lenovo Vantage / Legion Space'ten kapatın ya da LenovoLegionLinux'un legion_laptop modülünü yükleyin; bir EC sıfırlaması (kapatın, güç düğmesini ~30 sn basılı tutun) da temizler.&lt;/span&gt;</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1168" />
            <source>&lt;span style='color:%1'&gt;&lt;b&gt;EC Full Speed on&lt;/b&gt; — all fans at maximum&lt;/span&gt;</source>
            <translation>&lt;span style='color:%1'&gt;&lt;b&gt;EC Tam Hız açık&lt;/b&gt; — tüm fanlar azami hızda&lt;/span&gt;</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1169" />
            <source>&lt;span style='color:%1'&gt;&lt;b&gt;Max fans on&lt;/b&gt; — all fans at maximum&lt;/span&gt;</source>
            <translation>&lt;span style='color:%1'&gt;&lt;b&gt;Azami fanlar açık&lt;/b&gt; — tüm fanlar azami hızda&lt;/span&gt;</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1170" />
            <source>Disable full speed</source>
            <translation>Tam hızı kapat</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1190" />
            <source>All fans back to Auto — the EC curve takes over as they spin down.</source>
            <translation>Tüm fanlar Otomatik'e döndü — yavaşlarken EC eğrisi devralır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1221" />
            <source>Fan to Auto</source>
            <translation>Fanı Otomatik yap</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1222" />
            <source>The EC hands the fans back to its own curve only when ALL fan targets are Auto.

While another fan stays manual, this fan keeps running at its current speed (e.g. max) instead of slowing down. To slow only this fan, set a fixed RPM instead.</source>
            <translation>EC fanları kendi eğrisine yalnızca TÜM fan hedefleri Otomatik olduğunda geri verir.

Başka bir fan elle ayarlı kaldığı sürece bu fan yavaşlamak yerine mevcut hızında (örneğin azami) çalışmaya devam eder. Yalnızca bu fanı yavaşlatmak için bunun yerine sabit bir dev/dk ayarlayın.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1226" />
            <source>All fans to Auto</source>
            <translation>Tüm fanlar Otomatik</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1227" />
            <source>Only this fan</source>
            <translation>Yalnızca bu fan</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1229" />
            <source>Remember for this session</source>
            <translation>Bu oturum için hatırla</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1250" />
            <source>Fan set to Auto, but it keeps its last speed until every fan is on Auto (EC behaviour).</source>
            <translation>Fan Otomatik yapıldı, ancak her fan Otomatik olana kadar son hızını korur (EC davranışı).</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1270" />
            <source>The EC's Full Speed mode is on and this kernel cannot turn it off — see the note under the fans.</source>
            <translation>EC'nin Tam Hız modu açık ve bu çekirdek onu kapatamıyor — fanların altındaki nota bakın.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1298" />
            <source>%1: asked for %2, the firmware kept %3</source>
            <translation>%1: %2 istendi, donanım yazılımı %3 olarak bıraktı</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1339" />
            <source>%1 reached %2 °C with a fixed fan speed set: all fans are back on Auto (the EC curve).</source>
            <translation>%1, sabit fan hızı ayarlıyken %2 °C'ye ulaştı: tüm fanlar Otomatik'e (EC eğrisi) döndü.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1342" />
            <source>Fans back to Auto</source>
            <translation>Fanlar Otomatik'e döndü</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1369" />
            <source>No power-profile interface found. This usually means the lenovo-wmi-gamezone (or legion-laptop) platform driver is not loaded. CPU and GPU limits can still be set from the Firmware Attributes tab.</source>
            <translation>Güç profili arayüzü bulunamadı. Bu genellikle lenovo-wmi-gamezone (ya da legion-laptop) platform sürücüsünün yüklü olmadığı anlamına gelir. İşlemci ve GPU sınırları yine de Donanım Yazılımı Öznitelikleri sekmesinden ayarlanabilir.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1391" />
            <source>Could not read the current power profile.</source>
            <translation>Geçerli güç profili okunamadı.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1413" />
            <source>Switching to %1…</source>
            <translation>%1 profiline geçiliyor…</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1421" />
            <source>Authorization failed</source>
            <translation>Yetkilendirme başarısız</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1427" />
            <source>Could not switch profile</source>
            <translation>Profil değiştirilemedi</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1427" />
            <source>unknown error</source>
            <translation>bilinmeyen hata</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1434" />
            <source>Requested %1, but the firmware settled on %2.</source>
            <translation>%1 istendi, ancak donanım yazılımı %2 üzerinde karar kıldı.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1437" />
            <source>Power profile set to %1.</source>
            <translation>Güç profili %1 olarak ayarlandı.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1474" />
            <source>Could not resume scenes: %1</source>
            <translation>Sahneler sürdürülemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1481" />
            <source>&lt;b&gt;Boot presets paused&lt;/b&gt; — %1.</source>
            <translation>&lt;b&gt;Açılış ön ayarları duraklatıldı&lt;/b&gt; — %1.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1482" />
            <source>&lt;b&gt;Automatic login scene paused&lt;/b&gt; — %1.</source>
            <translation>&lt;b&gt;Otomatik oturum açma sahnesi duraklatıldı&lt;/b&gt; — %1.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1484" />
            <source>&lt;br&gt;&lt;span style='color:%1'&gt;Lower the offending undervolt / curve, then resume.&lt;/span&gt;</source>
            <translation>&lt;br&gt;&lt;span style='color:%1'&gt;Soruna yol açan undervolt / eğriyi düşürün, sonra sürdürün.&lt;/span&gt;</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1490" />
            <source>dGPU only</source>
            <translation>yalnızca dGPU</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1490" />
            <source>hybrid</source>
            <translation>hibrit</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1497" />
            <source>Not supported by this firmware.</source>
            <translation>Bu donanım yazılımı tarafından desteklenmiyor.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1507" />
            <source>  — after reboot (running %1)</source>
            <translation>  — yeniden başlatmadan sonra (şu an %1)</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1518" />
            <source>Switch to Hybrid at the next boot?

The AMD iGPU will drive the internal display; the NVIDIA GPU can power off when idle. Games run on NVIDIA via PRIME render offload (prime-run / __NV_PRIME_RENDER_OFFLOAD=1).

An X11 config that forces the NVIDIA GPU as primary can leave the desktop black in Hybrid — Wayland sessions are unaffected. If anything goes wrong, the BIOS setup (F2) has the same switch.</source>
            <translation>Bir sonraki açılışta Hibrit'e geçilsin mi?

Dahili ekranı AMD iGPU sürecek; NVIDIA GPU boştayken kapanabilir. Oyunlar PRIME render offload ile NVIDIA üzerinde çalışır (prime-run / __NV_PRIME_RENDER_OFFLOAD=1).

NVIDIA GPU'yu birincil yapmaya zorlayan bir X11 yapılandırması Hibrit'te masaüstünü siyah bırakabilir — Wayland oturumları etkilenmez. Bir şey ters giderse BIOS kurulumunda (F2) aynı anahtar bulunur.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1522" />
            <source>Switch to dGPU only at the next boot?

The internal display is wired straight to the NVIDIA GPU (lowest latency, G-SYNC on the panel). The iGPU is hidden and the NVIDIA GPU never powers off — shorter battery life.</source>
            <translation>Bir sonraki açılışta yalnızca dGPU'ya geçilsin mi?

Dahili ekran doğrudan NVIDIA GPU'ya bağlıdır (en düşük gecikme, panelde G-SYNC). iGPU gizlenir ve NVIDIA GPU hiç kapanmaz — pil ömrü kısalır.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1532" />
            <source>GPU mode — iGPU driver missing</source>
            <translation>GPU modu — iGPU sürücüsü eksik</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1533" />
            <source>

Switch anyway? Only do this if you are about to boot a kernel that has the iGPU driver, or you know how to switch back in the BIOS setup (F2).</source>
            <translation>

Yine de geçilsin mi? Bunu yalnızca iGPU sürücüsü olan bir çekirdekle açmak üzereyseniz ya da BIOS kurulumunda (F2) geri dönmeyi biliyorsanız yapın.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1540" />
            <source>The GPU mode was not changed:

%1</source>
            <translation>GPU modu değiştirilmedi:

%1</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1541" />
            <source>GPU mode set — reboot to switch.</source>
            <translation>GPU modu ayarlandı — geçmek için yeniden başlatın.</translation>
        </message>
        <message>
            <location filename="../src/hometab.cpp" line="1541" />
            <source>GPU mode unchanged.</source>
            <translation>GPU modu değişmedi.</translation>
        </message>
    </context>
    <context>
        <name>MainText</name>
        <message>
            <location filename="../src/main.cpp" line="243" />
            <source>Centurion — model not supported</source>
            <translation>Centurion — model desteklenmiyor</translation>
        </message>
        <message>
            <location filename="../src/main.cpp" line="244" />
            <source>This model (%1) is not supported yet.

Centurion has not been tested on it, and some firmware features may be missing or behave differently. Please send your system information on GitHub so support can be added.</source>
            <translation>Bu model (%1) henüz desteklenmiyor.

Centurion bunda test edilmedi; bazı donanım yazılımı özellikleri eksik olabilir ya da farklı davranabilir. Destek eklenebilmesi için sistem bilgilerinizi GitHub'da paylaşın.</translation>
        </message>
        <message>
            <location filename="../src/main.cpp" line="249" />
            <source>Report on GitHub</source>
            <translation>GitHub'da bildir</translation>
        </message>
        <message>
            <location filename="../src/main.cpp" line="250" />
            <source>Copy system info</source>
            <translation>Sistem bilgisini kopyala</translation>
        </message>
        <message>
            <location filename="../src/main.cpp" line="251" />
            <source>Continue</source>
            <translation>Devam</translation>
        </message>
        <message>
            <location filename="../src/main.cpp" line="254" />
            <source>Don't show again for this model</source>
            <translation>Bu model için bir daha gösterme</translation>
        </message>
    </context>
    <context>
        <name>MainWindow</name>
        <message>
            <location filename="../src/mainwindow.cpp" line="61" />
            <source>Home</source>
            <translation>Ana sayfa</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="67" />
            <source>Firmware</source>
            <translation>Donanım yazılımı</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="88" />
            <source>Ryzen Curve</source>
            <translation>Ryzen Eğrisi</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="91" />
            <source>Intel Undervolt</source>
            <translation>Intel Undervolt</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="95" />
            <location filename="../src/mainwindow.cpp" line="159" />
            <source>NVIDIA Curve</source>
            <translation>NVIDIA Eğrisi</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="100" />
            <source>AMD GPU</source>
            <translation>AMD GPU</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="103" />
            <source>Optimizations</source>
            <translation>Optimizasyonlar</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="107" />
            <location filename="../src/mainwindow.cpp" line="110" />
            <source>Lighting</source>
            <translation>Aydınlatma</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="113" />
            <source>Health</source>
            <translation>Sağlık</translation>
        </message>
        <message>
            <location filename="../src/mainwindow.cpp" line="124" />
            <source>Scenes</source>
            <translation>Sahneler</translation>
        </message>
    </context>
    <context>
        <name>ScenesTab</name>
        <message>
            <location filename="../src/scenestab.cpp" line="65" />
            <location filename="../src/scenestab.cpp" line="173" />
            <location filename="../src/scenestab.cpp" line="210" />
            <location filename="../src/scenestab.cpp" line="213" />
            <location filename="../src/scenestab.cpp" line="254" />
            <location filename="../src/scenestab.cpp" line="261" />
            <location filename="../src/scenestab.cpp" line="280" />
            <location filename="../src/scenestab.cpp" line="489" />
            <location filename="../src/scenestab.cpp" line="593" />
            <source>Unchanged</source>
            <translation>Değiştirme</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="86" />
            <source>Scenes</source>
            <translation>Sahneler</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="98" />
            <source>New scene…</source>
            <translation>Yeni sahne…</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="102" />
            <source>Export…</source>
            <translation>Dışa aktar…</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="103" />
            <source>Import…</source>
            <translation>İçe aktar…</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="128" />
            <source>Copy…</source>
            <translation>Kopyala…</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="130" />
            <source>Delete</source>
            <translation>Sil</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="142" />
            <location filename="../src/scenestab.cpp" line="529" />
            <source>Scene</source>
            <translation>Sahne</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="156" />
            <source>Power profile</source>
            <translation>Güç profili</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="162" />
            <source>Capture current</source>
            <translation>Mevcut durumu yakala</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="165" />
            <location filename="../src/scenestab.cpp" line="178" />
            <location filename="../src/scenestab.cpp" line="230" />
            <location filename="../src/scenestab.cpp" line="284" />
            <location filename="../src/scenestab.cpp" line="301" />
            <source>Clear</source>
            <translation>Temizle</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="168" />
            <source>Firmware limits</source>
            <translation>Donanım yazılımı sınırları</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="182" />
            <source>Fan curve</source>
            <translation>Fan eğrisi</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="185" />
            <source>Fan boost</source>
            <translation>Fan artırımı</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="195" />
            <source>GPU curve (NVIDIA)</source>
            <translation>GPU eğrisi (NVIDIA)</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="205" />
            <source>Optimizations</source>
            <translation>Optimizasyonlar</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="214" />
            <location filename="../src/scenestab.cpp" line="255" />
            <location filename="../src/scenestab.cpp" line="263" />
            <source>Off</source>
            <translation>Kapalı</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="221" />
            <source>Keyboard lighting</source>
            <translation>Klavye aydınlatması</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="227" />
            <location filename="../src/scenestab.cpp" line="281" />
            <source>Capture</source>
            <translation>Yakala</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="256" />
            <location filename="../src/scenestab.cpp" line="262" />
            <source>On</source>
            <translation>Açık</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="256" />
            <source>Level %1</source>
            <translation>Seviye %1</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="257" />
            <source>Keyboard light</source>
            <translation>Klavye ışığı</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="264" />
            <source>Panel Over Drive</source>
            <translation>Panel Over Drive</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="268" />
            <source>Refresh rate</source>
            <translation>Yenileme hızı</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="273" />
            <source>Run command</source>
            <translation>Komut çalıştır</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="292" />
            <source>Screen brightness</source>
            <translation>Ekran parlaklığı</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="310" />
            <source>Change on desktop…</source>
            <translation>Masaüstünde değiştir…</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="321" />
            <source>Shortcut</source>
            <translation>Kısayol</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="336" />
            <source>Save</source>
            <translation>Kaydet</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="337" />
            <source>Apply</source>
            <translation>Uygula</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="350" />
            <source>Automatic switching</source>
            <translation>Otomatik geçiş</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="356" />
            <source>Switch scenes with the power source</source>
            <translation>Sahneleri güç kaynağıyla değiştir</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="364" />
            <source>On battery</source>
            <translation>Pilde</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="368" />
            <source>Reapply the scene after waking from suspend</source>
            <translation>Uykudan uyanınca sahneyi yeniden uygula</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="385" />
            <source>Could not save the pause setting: %1</source>
            <translation>Duraklatma ayarı kaydedilemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="390" />
            <source>Show the active scene and power profile in games (MangoHud)</source>
            <translation>Etkin sahneyi ve güç profilini oyunlarda göster (MangoHud)</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="401" />
            <source>Could not change the MangoHud config: %1</source>
            <translation>MangoHud yapılandırması değiştirilemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="508" />
            <source>Reset curve</source>
            <translation>Eğriyi sıfırla</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="509" />
            <source>Restore originals</source>
            <translation>Orijinalleri geri yükle</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="678" />
            <source>Active</source>
            <translation>Etkin</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="696" />
            <source>Unsaved changes</source>
            <translation>Kaydedilmemiş değişiklikler</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="696" />
            <source>Save the changes to '%1' first?</source>
            <translation>Önce '%1' sahnesindeki değişiklikler kaydedilsin mi?</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="865" />
            <location filename="../src/scenestab.cpp" line="893" />
            <source>Could not save: %1</source>
            <translation>Kaydedilemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="880" />
            <source>Duplicate scene</source>
            <translation>Sahneyi çoğalt</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="880" />
            <source>New scene</source>
            <translation>Yeni sahne</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="901" />
            <source>Delete scene</source>
            <translation>Sahneyi sil</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="901" />
            <source>Delete scene '%1'?

The profiles it refers to are kept.</source>
            <translation>'%1' sahnesi silinsin mi?

Başvurduğu profiller korunur.</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="944" />
            <source>Could not save the automatic switching setting: %1</source>
            <translation>Otomatik geçiş ayarı kaydedilemedi: %1</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="953" />
            <source>Now on AC power</source>
            <translation>Şu an şebeke gücünde</translation>
        </message>
        <message>
            <location filename="../src/scenestab.cpp" line="953" />
            <source>Now on battery</source>
            <translation>Şu an pilde</translation>
        </message>
    </context>
    <context>
        <name>Tray</name>
        <message>
            <location filename="../src/tray.cpp" line="66" />
            <source>GPU curve guard</source>
            <translation>GPU eğri koruması</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="66" />
            <source>CPU curve guard</source>
            <translation>İşlemci eğri koruması</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="68" />
            <source>
Was: '%1'</source>
            <translation>
Önceki: '%1'</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="68" />
            <source>Undo</source>
            <translation>Geri al</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="71" />
            <location filename="../src/tray.cpp" line="253" />
            <source>Applying profile '%1'…</source>
            <translation>'%1' profili uygulanıyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="75" />
            <source>Profile '%1' applied again.</source>
            <translation>'%1' profili yeniden uygulandı.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="75" />
            <location filename="../src/tray.cpp" line="212" />
            <location filename="../src/tray.cpp" line="232" />
            <source>Profile '%1': %2</source>
            <translation>Profil '%1': %2</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="83" />
            <source>%1 game detected</source>
            <translation>%1 oyunu algılandı</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="83" />
            <source>%1 — game mode started.</source>
            <translation>%1 — oyun modu başladı.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="86" />
            <source>%1 game closed</source>
            <translation>%1 oyunu kapandı</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="86" />
            <source>%1 — game mode ended.</source>
            <translation>%1 — oyun modu bitti.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="93" />
            <source>Settings changed outside Centurion</source>
            <translation>Ayarlar Centurion dışında değiştirildi</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="95" />
            <source>
Running: %1</source>
            <translation>
Çalışıyor: %1</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="96" />
            <source>
Optimizations tab → Re-apply.</source>
            <translation>
Optimizasyonlar sekmesi → Yeniden uygula.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="97" />
            <source>Re-apply</source>
            <translation>Yeniden uygula</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="108" />
            <source>Details</source>
            <translation>Ayrıntılar</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="109" />
            <source>Scenes</source>
            <translation>Sahneler</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="112" />
            <location filename="../src/tray.cpp" line="117" />
            <source>Scene: %1</source>
            <translation>Sahne: %1</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="112" />
            <source>Applied.</source>
            <translation>Uygulandı.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="115" />
            <location filename="../src/tray.cpp" line="121" />
            <source>Centurion — scene: %1</source>
            <translation>Centurion — sahne: %1</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="117" />
            <source>Game scene: %1</source>
            <translation>Oyun sahnesi: %1</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="118" />
            <source>Game started — scene switched.</source>
            <translation>Oyun başladı — sahne değişti.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="118" />
            <source>Scene switched.</source>
            <translation>Sahne değişti.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="120" />
            <source>Centurion — applying scene '%1'…</source>
            <translation>Centurion — '%1' sahnesi uygulanıyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="132" />
            <source>Still applying the previous change — try again in a moment.</source>
            <translation>Önceki değişiklik hâlâ uygulanıyor — birazdan yeniden deneyin.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="153" />
            <location filename="../src/tray.cpp" line="161" />
            <source>Scene</source>
            <translation>Sahne</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="161" />
            <source>Applying '%1'…</source>
            <translation>'%1' uygulanıyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="165" />
            <source>Switch with power source</source>
            <translation>Güç kaynağıyla değiştir</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="170" />
            <source>Choose the AC / battery scenes in the Scenes tab first</source>
            <translation>Önce Sahneler sekmesinde şebeke / pil sahnelerini seçin</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="176" />
            <source>Pause scenes</source>
            <translation>Sahneleri duraklat</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="179" />
            <source>No automatic scene changes (power source, login, game) until unchecked</source>
            <translation>İşaret kaldırılana kadar otomatik sahne değişikliği yok (güç kaynağı, oturum açma, oyun)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="186" />
            <source>Power Profile</source>
            <translation>Güç Profili</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="189" />
            <source>(no platform-profile driver)</source>
            <translation>(platform profili sürücüsü yok)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="203" />
            <source>CPU Curve (Ryzen)</source>
            <translation>İşlemci Eğrisi (Ryzen)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="205" />
            <location filename="../src/tray.cpp" line="227" />
            <location filename="../src/tray.cpp" line="248" />
            <source>(no saved profiles)</source>
            <translation>(kayıtlı profil yok)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="212" />
            <location filename="../src/tray.cpp" line="219" />
            <source>CPU curve</source>
            <translation>İşlemci eğrisi</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="212" />
            <location filename="../src/tray.cpp" line="232" />
            <source>Profile '%1' applied.</source>
            <translation>'%1' profili uygulandı.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="216" />
            <location filename="../src/tray.cpp" line="256" />
            <source>Reset curve</source>
            <translation>Eğriyi sıfırla</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="219" />
            <source>Resetting Curve Optimizer (coall=0)…</source>
            <translation>Curve Optimizer sıfırlanıyor (coall=0)…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="225" />
            <source>CPU Undervolt (Intel)</source>
            <translation>İşlemci Undervolt (Intel)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="232" />
            <location filename="../src/tray.cpp" line="239" />
            <source>CPU undervolt</source>
            <translation>İşlemci undervolt</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="236" />
            <source>Reset voltages</source>
            <translation>Voltajları sıfırla</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="239" />
            <source>Resetting voltage offsets to 0 mV…</source>
            <translation>Voltaj ofsetleri 0 mV'ye sıfırlanıyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="245" />
            <source>GPU Curve (NVIDIA)</source>
            <translation>GPU Eğrisi (NVIDIA)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="253" />
            <source>GPU curve</source>
            <translation>GPU eğrisi</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="263" />
            <location filename="../src/tray.cpp" line="269" />
            <location filename="../src/tray.cpp" line="270" />
            <location filename="../src/tray.cpp" line="278" />
            <source>Optimizations</source>
            <translation>Optimizasyonlar</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="269" />
            <source>Applying preset '%1'…</source>
            <translation>'%1' ön ayarı uygulanıyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="270" />
            <source>Preset '%1' has nothing applicable here.</source>
            <translation>'%1' ön ayarında burada uygulanabilecek bir şey yok.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="273" />
            <source>Restore originals</source>
            <translation>Orijinalleri geri yükle</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="278" />
            <source>Restoring original values…</source>
            <translation>Orijinal değerler geri yükleniyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="284" />
            <source>Keyboard Lighting</source>
            <translation>Klavye Aydınlatması</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="286" />
            <source>(keyboard not read — see the Lighting tab)</source>
            <translation>(klavye okunamadı — Aydınlatma sekmesine bakın)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="289" />
            <source>Profile %1</source>
            <translation>Profil %1</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="293" />
            <source>Keyboard lighting</source>
            <translation>Klavye aydınlatması</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="293" />
            <source>The Lighting tab has changes not applied yet — apply or revert them first.</source>
            <translation>Aydınlatma sekmesinde henüz uygulanmamış değişiklikler var — önce uygulayın ya da geri alın.</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="298" />
            <source>Lights off</source>
            <translation>Işıklar kapalı</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="302" />
            <source>Brightness</source>
            <translation>Parlaklık</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="316" />
            <location filename="../src/tray.cpp" line="323" />
            <location filename="../src/tray.cpp" line="324" />
            <location filename="../src/tray.cpp" line="329" />
            <location filename="../src/tray.cpp" line="343" />
            <source>Fans</source>
            <translation>Fanlar</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="318" />
            <source>Max all fans</source>
            <translation>Tüm fanlar azami</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="323" />
            <location filename="../src/tray.cpp" line="329" />
            <source>All fans back to Auto…</source>
            <translation>Tüm fanlar Otomatik'e dönüyor…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="324" />
            <source>All fans to maximum…</source>
            <translation>Tüm fanlar azami hıza…</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="326" />
            <source>All fans to Auto</source>
            <translation>Tüm fanlar Otomatik</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="333" />
            <location filename="../src/tray.cpp" line="350" />
            <source>%1 RPM</source>
            <translation>%1 dev/dk</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="333" />
            <location filename="../src/tray.cpp" line="346" />
            <source>Auto</source>
            <translation>Otomatik</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="343" />
            <source>%1 → %2 RPM</source>
            <translation>%1 → %2 dev/dk</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="343" />
            <source>%1 → Auto</source>
            <translation>%1 → Otomatik</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="351" />
            <source>%1 RPM (max)</source>
            <translation>%1 dev/dk (azami)</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="357" />
            <source>Hide window</source>
            <translation>Pencereyi gizle</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="357" />
            <source>Show window</source>
            <translation>Pencereyi göster</translation>
        </message>
        <message>
            <location filename="../src/tray.cpp" line="358" />
            <source>Quit</source>
            <translation>Çık</translation>
        </message>
    </context>
</TS>
