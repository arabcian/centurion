# Türkçe yerelleştirme — doğrulanmış kapsam

Bu sürümde Optimization ekranının Rust `tune.rs` kaynağından gelen **196/196 teknik
`help` açıklaması eksiksiz Türkçeye çevrildi**. Geçici "Türkçe özet + İngilizce
orijinal" yöntemi kullanılmaz; seçeneklerin teknik eşikleri, uyarıları,
komutları ve yol adları korunur.

- 196 ayar etiketi, 8 grup başlığı ve temel seçenek etiketleri çevrilir.
- 22 uzun Optimization ekranı tooltipi ile çok sayıda menü/düğme etiketi
  Türkçe gösterilir.
- Backend'e gönderilen JSON anahtarları, seçenek `itemData` değerleri, dosya
  yolları ve preset kimlikleri **değiştirilmedi**.
- Arama/sayfa gezinmesi Türkçeleştirilen grup başlıklarına bağlı olmaktan
  çıkarıldı; dahili grup kimlikleriyle çalışır.
- `QT_TRANSLATE_NOOP` işaretleyicileri, çalışma anında Rust'tan gelen
  kaynakların `lupdate` sonrasında katalogdan düşmesini önler.
- Test: `python3 gui/tests/translation_tr_test.py`

**Sınır:** Optimization dışındaki tüm ekranlarda eksiksiz Türkçe iddia
edilmemektedir. Bu kaynakta daha önceden de kısmen İngilizce kalmış ekranlar
vardır. Bu ortamda Qt 6 geliştirme paketleri bulunmadığından uygulamanın C++
derlemesi veya `.qm` dosyası üretimi doğrulanamadı. Gentoo ebuild'i Qt
LinguistTools gereksinimini zaten belirtmektedir.
