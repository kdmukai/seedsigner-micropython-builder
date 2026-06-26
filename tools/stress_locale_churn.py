# Multi-language screen-exercise / animation stress harness  (Approach A; docs/approach-a-cache-psram-design.md)
# Exercises ALL 5 screen types across every supported language with REALISTIC shapes, plus targeted
# animation/scroll stress. Deep memory validation is already done (repro_realistic_nav.py 61/61; earlier
# heavy run converged); this confirms the cache-node FREE path across the full language set AND drives the
# animation hot path (scroll + pulsing edges) that re-traverses the glyph cache in PSRAM.
#
# Per locale (per pass): short menu (button_list) | a number of PARAGRAPH screens, alternating the two real
# long-text vehicles -- button_list TEXT field, and large_icon_status body (progressive body-glyph load) |
# the full settings menu as a LONG scrollable button_list (real localized setting names) | passphrase.
# Once per pass: main_menu + screensaver, then a TORTURE block (zh / CJK) that DWELLS ~2s so the LVGL
# port task actually runs the animations:
#   - all 4 large_icon_status types (success/warning/dire_warning/error) WITH warning_edges flashing +
#     a long autoscrolling headline
#   - a forced-scroll top_nav title
#   - a button_list with one over-long option (forced label scroll)
# Dwell matters: animations run on the esp_lvgl_port task, so sleeping lets them advance; just building the
# label only tests initial rasterization (alloc), not the animation re-traversal (the hot-path risk).
#
# Run (self-contained, on-device):  mpremote connect <port> run tools/stress_locale_churn.py
import esp
esp.osdebug(None)
import json, machine, vfs, os, time as t
import seedsigner_lvgl_screens as ss
ss.init(); ss.set_screensaver_timeout(0)
try: os.listdir('/sd')
except OSError: vfs.mount(vfs.VfsFat(machine.SDCard(slot=0, width=4)), '/sd')

LOCS = ["en", "el", "ru", "vi", "zh_Hans_CN", "ja", "ko", "fa", "hi", "th", "ur"]
DATA = {"en": {"title": "Settings", "btn": ["tBtc", "tSats", "btc", "sats", "SD card removed", "SD card inserted", "Review Transaction", "Review details"], "para": ["You can remove the SD card now", "Click to approve this transaction", "SeedSigner is restarting.  All in-memory data will be wiped.", "It is safe to disconnect power at any time.", "Optionally verify that your mnemonic backup is correct.", "Optionally scan your transcribed SeedQR to confirm that it reads back correctly.", "Load your multisig wallet descriptor to verify your receive/self-transfer or change address.", "SeedSigner is 100% free & open source, funded solely by the Bitcoin community.  Donate onchain or LN at:"], "settings": ["Language", "Mnemonic language", "Persistent settings", "Denomination display", "Bitcoin network", "QR code density", "Sig types", "Script types", "Xpub QR format", "Show xpub details", "BIP-39 passphrase", "Camera rotation", "Compact SeedQR", "BIP-85 child seeds", "Electrum seeds", "MicroSD notification duration", "Message signing", "Show privacy warnings", "Show dire warnings", "Show QR brightness tips", "Show partner logos", "Display type", "Invert colors", "QR background color"], "menu_title": "Home", "menu_btns": ["Scan", "Seeds", "Tools", "Settings"]}, "el": {"title": "Ρυθμίσεις", "btn": ["tBtc", "tSats", "btc", "sats", "1 είσοδος", "είσοδος 1", "είσοδος 2", "[ ... ]"], "para": ["Μπορείς τώρα να αφαιρέσεις την κάρτα SD", "{} ρέστα-διεύθυνση ρέστων", "Η διεύθυνση επαληθεύθηκε!", "Κάνε κλίκ για έγκριση αυτής της συναλλαγής", "Το SeedSigner επανεκκινείται.  Όλα τα δεδομένα στην μνήμη θα διαγραφούν", "Μπορείς με ασφάλεια να αποσυνδέσεις την τροφοδοσία οποιαδήποτε στιγμή", "Επαλήθευση αντιγράφου ασφαλείας;", "Προαιρετικά επαλήθευσε οτι το mnemonic αντίγραφο είναι σωστό"], "settings": ["Γλώσσα", "Γλώσσα mnemonic", "Μόνιμες ρυθμίσεις", "Εμφάνιση μονάδων", "Δίκτυο Bitcoin", "Πυκνότητα QR code", "Τύποι υπογραφών", "Τύποι script", "Xpub QR format", "Εμφάνιση λεπτομερειών xPub", "Συνθηματική φράση BIP-39", "Περιστροφή κάμερα", "Συμπαγές SeedQR", "Παράγωγα seeds BIP-85", "Electrum seeds", "MicroSD notification duration", "Υπογραφή μηνύματος", "Εμφάνιση προειδοποιήσεων ιδιωτικότητας", "Εμφάνιση κρίσιμων προειδοποιήσεων", "Εμφάνιση συμβουλών φωτεινότητας QR", "Εμφάνιση λογοτύπων συνεργατών", "Display type", "Invert colors", "Χρώμα φόντου QR"], "menu_title": "Αρχική", "menu_btns": ["Σάρωση", "Seeds", "Εργαλεία", "Ρυθμίσεις"]}, "ru": {"title": "Настр.", "btn": ["tBtc", "tSats", "btc", "sats", "SD-карта извлечена", "SD-карта вставлена", "1 вход", "вход 1"], "para": ["Теперь вы можете извлечь SD-карту", "необработанные шестнадцатеричные данные", "Нажмите, чтобы подтвердить эту транзакцию", "SeedSigner перезагружается.  Все данные в памяти будут удалены.", "Отключить электропитание можно в любое время.", "При необходимости, подтвердите что ваша резервная копия мнемоники правильная.", "Индексы списка слов BIP-39", "Необработанные биты энтропии"], "settings": ["Язык", "Язык мнемоники", "Постоянные настр.", "Отобр. номинала", "Биткоин-сеть", "Плотность QR-кода", "Типы подписи", "Типы скриптов", "Xpub QR format", "Показать детали xpub", "Пароль. фраза BIP-39", "Вращение камеры", "Компактный СидQR", "Дочерние сиды BIP-85", "Сиды Electrum", "MicroSD notification duration", "Подпись сообщения", "Показать предупреждения о конфиденциальности", "Показать крайние предупреждения", "Показать подсказки яркости QR", "Показать логотипы партнеров", "Display type", "Invert colors", "Цвет фона QR-кода"], "menu_title": "Главная", "menu_btns": ["Сканир.", "Сиды", "Инстр.", "Настр."]}, "vi": {"title": "Cài đặt", "btn": ["tBtc", "tSats", "btc", "sats", "thẻ SD đã tháo", "thẻ SD đã cài", "Kiểm tra giao dịch", "Kiểm tra thông tin"], "para": ["Bây giờ bạn có thể lấy thẻ SD ra", "Nhấn vào để chấp thuận giao dịch", "SeedSigner đang khởi động lại.  Tất cả dữ liệu trong bộ nhớ sẽ bị xóa.", "Ngắt kết nối với thiết bị", "An toàn để ngắt nguồn điện bất cứ lúc nào", "Hoàn tất cài đặt cụm từ khóa", "Tùy chọn xác nhận rằng các từ khóa dự phòng đúng", "Danh sách từ chỉ mục BIP-39"], "settings": ["Ngôn ngữ", "Ngôn ngữ cho từ khóa", "Cài đặt cố định", "Đơn vị hiển thị", "Mạng Bitcoin", "Mật độ mã QR", "Loại chữ ký", "Loại lệnh", "Xpub QR format", "Hiển thị chi tiết xpub", "Cụm từ khóa BIP-39", "Xoay camera", "Mã SeedQR tinh gọn", "Các từ khóa thứ cấp BIP-85", "Các từ khóa Electrum", "MicroSD notification duration", "Ký lời nhắn", "Hiển thị cảnh báo bảo mật", "Hiển thị cảnh báo nguy hiểm", "Hiển thị hướng dẫn tùy chỉnh độ sáng cho mã QR", "Hiển thị thương hiệu của đối tác", "Hiển thị loại", "Đảo màu", "Màu nền mã QR"], "menu_title": "Trang chủ", "menu_btns": ["Quét", "Từ khóa", "Công cụ", "Cài đặt"]}, "zh_Hans_CN": {"title": "设置", "btn": ["tBtc", "tSats", "btc", "sats", "您现在可以取出 SD卡了", "SD 卡已移除", "SD 卡已插入", "审核交易"], "para": ["SeedSigner 正在重启。  内存数据将被清除。", "可选择扫描您抄写的 SeedQR，以确认是否读取正确", "加载您的多重签名钱包描述符，以验证您的接收、自我转账或找零地址。", "SeedSigner 是 100% 自由和开源，由社区独立资助。  链上或闪电网络捐赠：", "第 {mnemonic_length} 个助记词由 {num_bits} 个熵位及自动校验码生成", "此交易将花费全部输入金额，不会有找零返回到您的钱包。", "某些功能对于 Electrum 格式的助记词是被禁用的", "您必须将您的助记词保密的保管，并远离所有联网的设备。"], "settings": ["语言", "助记词语言", "持久性设置", "单位显示", "比特币网络", "二维码密度", "签名类型", "脚本类型", "Xpub 二维码格式", "显示 xpub 详情", "BIP-39 密码短语", "摄像头旋转", "紧凑型 SeedQR", "BIP-85 子助记词", "Electrum 助记词", "MicroSD 卡通知显示时长", "留言签署", "显示隐私警告", "显示严重警告", "显示二维码亮度提示", "显示合作伙伴商标", "显示器类型", "反转颜色", "二维码背景颜色"], "menu_title": "主页", "menu_btns": ["扫描", "助记词", "工具", "设置"]}, "ja": {"title": "設定", "btn": ["tBtc", "tｻﾄｼ", "btc", "ｻﾄｼ", "SDｶｰﾄﾞを抜いても良い", "SDｶｰﾄﾞが抜かれました", "SDｶｰﾄﾞが挿入されました", "取引を確認"], "para": ["ｼｰﾄﾞｻｲﾅｰが再起動中。  揮発性ﾒﾓﾘｰにある全てのﾃﾞｰﾀが削除されます。", "ﾊﾞｯｸｱｯﾌﾟに誤りがないかを確認する（省略可）。", "書き写されたｼｰﾄﾞQRの控えを読み取り、誤りがないかを確認する（省略可）。", "ﾏﾙﾁｼｸﾞｳｫﾚｯﾄのdescriptorを入力すれば、お釣り用ｱﾄﾞﾚｽ（或いは自分に送金する場合の入金用ｱﾄﾞﾚｽ）を確認できる。", "ｼｰﾄﾞｻｲﾅｰは100%ﾎﾞﾗﾝﾃｨｱによるｵｰﾌﾟﾝｿｰｽｿﾌﾄｳｪｱです。ご支援下さる方は下記ｳｪﾌﾞｻｲﾄから、ﾋﾞｯﾄｺｲﾝ（ﾗｲﾄﾆﾝｸﾞも含む）にて寄付ができます。", "{mnemonic_length}番目のﾜｰﾄﾞを、更なる{num_bits}ﾋﾞｯﾄのｴﾝﾄﾛﾋﾟｰと、自動計算されるﾁｪｯｸｻﾑとで組む。 ", "取引のｲﾝﾌﾟｯﾄに未対応のｽｸﾘﾌﾟﾄがあるため、お釣り用ｱﾄﾞﾚｽを手動で確認する必要がある。", "ｲﾝﾌﾟｯﾄの全額が支払われるため、お釣りはありません。"], "settings": ["言語", "ｼｰﾄﾞﾜｰﾄﾞ言語", "非揮発性設定", "表示金額の単位", "ﾋﾞｯﾄｺｲﾝﾈｯﾄﾜｰｸ", "QRｺｰﾄﾞの解像度", "署名の書類", "ｽｸﾘﾌﾟﾄの種類", "xpubQRの形式", "xpubの詳細を表示", "BIP-39ﾊﾟｽﾌﾚｰｽﾞ", "ｶﾒﾗ回転", "ｺﾝﾊﾟｸﾄなｼｰﾄﾞQR", "BIP-85の子ｼｰﾄﾞ", "Electrum用ｼｰﾄﾞ", "SDｶｰﾄﾞ通知期間", "ﾒｯｾｰｼﾞの署名", "個人情報漏洩注意の表示", "深刻危険注意の表示", "QR輝度ﾋﾝﾄの表示", "ﾊﾟｰﾄﾅｰ ﾛｺﾞの表示", "ﾃﾞｨｽﾌﾟﾚｲの種類", "色の反転", "QR背景色"], "menu_title": "メニュー", "menu_btns": ["読み取る", "ｼｰﾄﾞ箱", "道具箱", "設定"]}, "ko": {"title": "설정", "btn": ["tBtc", "tSats", "btc", "sats", "SD 카드 제거 가능", "SD 카드 제거됨", "SD 카드 삽입됨", "트랜잭션 확인"], "para": ["시드사이너가 재시작됩니다.   메모리에 있는 모든 데이터는 삭제됩니다.", "(선택) 기록한 시드QR을 스캔해 올바른지 확인합니다.", "다중서명 지갑 descriptor를 불러 '나에게 송금'을 포함한 입금·잔돈 주소를 확인합니다.", "시드사이너는 100% 무료 오픈 소스이며, 비트코인 커뮤니티의 후원으로 운영됩니다.  온체인 또는 LN 기부:", "{mnemonic_length}번째 단어는 {num_bits}비트의 추가 엔트로피와 자동 계산된 체크섬으로 구성됩니다.", "지원되지 않는 입력 스크립트가 있습니다. 잔돈 주소를 확인하세요.", "지갑 descriptor에서 {} 주소를 검증할 수 없습니다.", "인식할 수 없거나 아직 지원되지 않는 QR 코드입니다."], "settings": ["언어", "니모닉 언어", "설정 유지", "단위 표기", "비트코인 네트워크", "QR코드 해상도", "서명 유형", "스크립트 유형", "Xpub QR 형식", "Xpub 상세 보기", "BIP-39 패스프레이즈", "카메라 회전", "콤팩트 시드QR", "BIP-85 자식 시드", "일렉트럼 시드", "MicroSD 알림 표시 시간", "메시지 서명", "프라이버시 경고 표시", "심각한 경고 표시", "QR 밝기 안내 표시", "파트너 로고 표시", "디스플레이 유형", "색상 반전", "QR 배경색"], "menu_title": "메뉴", "menu_btns": ["스캔", "시드", "도구", "설정"]}, "fa": {"title": "تنظیمات", "btn": ["بیت‌کوین آزمایشی", "ساتوشی آزمایشی", "بیت‌کوین", "ساتوشی", "بازبینی تراکنش", "بازبینی جزئيات", "1 ورودی", "ورودی 1"], "para": ["می‌توانید کارت حافظه را از دستگاه خارج کنید!", "برای تأیید این تراکنش کلیک کنید", "امکان افشای اطلاعت حریم خصوصی!", "دستگاه در حال راه اندازی مجدد است. تمام اطلاعات موجود در حافظه پاک خواهند شد.", "دستگاه را از برق خارج کنید.", "می‌توانید دستگاه را از برق خارج کنید.", "کلمات عبارت‌ بازیابی: {}/{}", "نسخه پشتیبان را تأیید می کنید؟"], "settings": ["زبان", "زبان کلمات دانه", "تنظیمات ماندگار", "نمایش واحد پول", "شبکه بیت‌کوین", "تراکم کد QR", "انواع امضا", "انواع اسکریپت", "فرمت کد QR برای Xpub", "نمایش جزئیات xpub", "رمز عبور BIP-39", "چرخش دوربین", "SeedQR فشرده", "عبارات بازیابی مرتبط با فرزند‌های تولید شده طبق BIP-85", "عبارات بازیابی الکتروم", "مدت زمان نمایش اعلان", " امضا پیام", "نمایش هشدارهای حریم خصوصی", "نمایش هشدارهای حیاتی", "نمایش راهنمای روشنایی QR", "نمایش لوگوی همکاران", "نوع نمایش صفحه", "معکوس کردن رنگ‌ها", "رنگ پس زمینه QR"], "menu_title": "خانه", "menu_btns": ["اسکن", "عبارات بازیابی", "ابزارها", "تنظیمات"]}, "hi": {"title": "सेटिंग", "btn": ["tBtc", "tSats", "btc", "sats", "SD कार्ड लगाया गया", "लेन-देन जाँचें", "विवरण जाँचें", "1 इनपुट"], "para": ["अब आप SD कार्ड निकाल सकते हैं", "इस लेन-देन की पुष्टि करने के लिए क्लिक करें", "SeedSigner रीस्टार्ट हो रहा है।  सारा डेटा मेमोरी से मिट जाएगा।", "किसी भी समय पावर हटाना सुरक्षित है।", "चाहें तो सत्यापित करें कि आपका निमोनिक बैकअप सही है।", "चाहें तो अपने उतारे गए SeedQR को स्कैन करके सत्यापित करें कि वह सही है।", "अपने प्राप्ति/सेल्फ-ट्रांसफर या चेंज पते को सत्यापित करने के लिए अपना मल्टीसिग वॉलेट डिस्क्रिप्टर लोड करें।", "डिस्क्रिप्टर लोड किया गया"], "settings": ["भाषा", "निमोनिक भाषा", "स्थायी सेटिंग", "मूल्यवर्ग प्रदर्शन", "बिटकॉइन नेटवर्क", "QR कोड डेंसिटी", "सिग्नेचर के प्रकार", "स्क्रिप्ट के प्रकार", "Xpub QR प्रारूप", "Xpub विवरण दिखाएं", "BIP-39 पासफ्रेज़", "कैमरा घुमाये", "कॉम्पैक्ट SeedQR", "BIP-85 चाइल्ड सीड्स", "इलेक्ट्रम सीड्स", "MicroSD सूचना की अवधि", "संदेश हस्ताक्षर", "निजता चेतावनी दिखाएं", "गंभीर चेतावनी दिखाएं", "QR रोशनी के सुझाव दिखाएं", "पार्टनर लोगो दिखाएं", " डिस्प्ले प्रकार", "रंग उल्टे करें", "QR का पीछे का रंग"], "menu_title": "होम", "menu_btns": ["स्कैन", "सीड्स", "टूल्स", "सेटिंग"]}, "th": {"title": "ตั้งค่า", "btn": ["tBtc", "tSats", "btc", "sats", "ถอดการ์ด SD แล้ว", "ใส่การ์ด SD แล้ว", "ตรวจสอบธุรกรรม", "ตรวจสอบรายละเอียด"], "para": ["กดเพื่ออนุมัติ การทำธุรกรรมนี้", "SeedSigner กำลังรีสตาร์ท  ข้อมูลในหน่วยความจำ จะถูกล้างทั้งหมด", "ตัดไฟได้ทันที โดยไม่เป็นอันตราย", "ตรวจสอบการสำรอง คำกู้คืนของคุณ หากต้องการ", "สแกน SeedQR ที่แปลงข้อมูลไว้ เพื่อยืนยันความถูกต้อง", "โหลดคำอธิบายกระเป๋าเงิน แบบหลายลายเซ็นเพื่อตรวจสอบ ที่อยู่รับเงิน / ส่งหาตัวเอง  หรือที่อยู่เงินทอน", "คำอธิบายกระเป๋าเงินถูกโหลดแล้ว", " SeedSigner ฟรีและโอเพนซอร์ส 100%  สนับสนุนโดยชุมชนบิตคอยน์ เท่านั้น ร่วมบริจาคผ่าน On-chain หรือ LN ได้ที่:"], "settings": ["ภาษา", "ภาษาชุดคำกู้คืน", "การตั้งค่าคงอยู่", "การตั้งค่าหน่วยเงิน", "เครือข่ายบิตคอยน์", "ความเข้มของ QR code", "ประเภทลายเซ็น", "ประเภทสคริปต์", "รูปแบบ QR ของ Xpub", "แสดงรายละเอียด xpub", "รหัสเสริม BIP-39", "เปลี่ยนมุมกล้อง", "Seed QR แบบย่อ", "Seed ย่อย มาตรฐาน BIP-85", "Seeds ของ Electrum", "ระยะเวลาแจ้งเตือน MicroSD", "เซ็นข้อความ", "แสดงคำเตือน เกี่ยวกับข้อมูลส่วนตัว", "แสดงคำเตือนร้ายแรง", "แสดงคำแนะนำปรับแสง QR", "แสดงโลโก้พันธมิตร", "ประเภทหน้าจอ", "สลับสีหน้าจอ", "สีพื้นหลังของ QR"], "menu_title": "หน้าหลัก", "menu_btns": ["สแกน", "Seeds", "เครื่องมือ", "ตั้งค่า"]}, "ur": {"title": "احتیاط", "btn": ["احتیاط", "ٹھیک ہے", "اسکین کریں", "میں سمجھ گیا"], "para": [], "settings": ["Language", "Mnemonic language", "Persistent settings", "Denomination display", "Bitcoin network", "QR code density", "Sig types", "Script types", "Xpub QR format", "Show xpub details", "BIP-39 passphrase", "Camera rotation", "Compact SeedQR", "BIP-85 child seeds", "Electrum seeds", "MicroSD notification duration", "Message signing", "Show privacy warnings", "Show dire warnings", "Show QR brightness tips", "Show partner logos", "Display type", "Invert colors", "QR background color"], "menu_title": "Home", "menu_btns": ["Scan", "Seeds", "Tools", "Settings"]}}
TORTURE = {"title": "设置", "long_title": "第 {mnemonic_length} 个助记词由 {num_bits} 个熵位及自动校验码生成", "long_btn": "加载您的多重签名钱包描述符，以验证您的接收、自我转账或找零地址。", "body": "SeedSigner 是 100% 自由和开源，由社区独立资助。  链上或闪电网络捐赠：", "short": ["tBtc", "tSats", "btc"]}

PASSES = 2
DWELL = 2000          # ms: let port-task animations (scroll, pulsing edges) actually run
class Stop(Exception): pass
USED_MAX = 90; BIG_MIN = 10000

def m(): return ss.mem_stats()
def line(tag, s, extra=''):
    print('%-17s used=%2d%% max=%-6d frag=%2d%% big=%-7d | rb live=%-4d alloc=%-6d fb=%d | spi_min=%-9d %s' % (
        tag, s['lvgl_used_pct'], s['lvgl_max_used'], s['lvgl_frag_pct'], s['lvgl_free_biggest'],
        s['rb_psram_live_nodes'], s['rb_psram_alloc'], s['rb_psram_fallback'], s['spiram_min_free'], extra))

def loadL(loc):
    try:
        f = json.loads(ss.locale_pack_files(loc)); p = {x: open('/sd/' + loc + '/' + x, 'rb').read() for x in f}
        return 1 if ss.load_locale(loc, p) else 0
    except Exception:
        return 0

def blist(title, btns, text=None, dwell=40):
    if not btns: return
    ss.clear_result_queue()
    cfg = {'top_nav': {'title': title or ' ', 'show_back_button': True, 'show_power_button': False},
           'button_list': btns}
    if text: cfg['text'] = text
    ss.button_list_screen(cfg); t.sleep_ms(dwell)

def status(title, body, headline=None, stype='warning', edges=False, dwell=40):
    if not body: return
    ss.clear_result_queue()
    ss.large_icon_status_screen({'top_nav': {'title': (title or ' ')[:14],
        'show_back_button': False, 'show_power_button': False},
        'status_type': stype, 'status_headline': headline or (title or ' ')[:14], 'text': body,
        'button_list': ['OK'], 'is_bottom_list': True, 'warning_edges': edges}); t.sleep_ms(dwell)

def passphrase(title, mode=None):
    ss.clear_result_queue()
    cfg = {'top_nav': {'title': (title or ' ')[:14], 'show_back_button': True, 'show_power_button': False}}
    if mode: cfg['initial_mode'] = mode   # 'upper' | 'digits' | 'symbols' (SPECIAL = the 32-symbol page)
    ss.seed_add_passphrase_screen(cfg); t.sleep_ms(40)

def torture():
    T = TORTURE
    loadL('zh_Hans_CN')   # load the CJK font FIRST so the zh torture glyphs render (else tofu boxes)
    for st in ('success', 'warning', 'dire_warning', 'error'):
        try: status(T['title'], T['body'], headline=T['long_title'], stype=st, edges=True, dwell=DWELL)
        except Exception: pass
    try: blist(T['long_title'], T['short'], dwell=DWELL)                 # forced-scroll top_nav title
    except Exception: pass
    try: blist(T['title'], [T['long_btn']] + T['short'], dwell=DWELL)    # over-long button: WRAPS to a tall button (buttons don't scroll by design)
    except Exception: pass

line('baseline', m())
try:
    for p in range(PASSES):
        for loc in LOCS:
            D = DATA[loc]; title = D['title']; btn = D['btn']; paras = D['para']
            ok = loadL(loc); fails = 0; peak = [0]; minbig = [1 << 30]
            def chk():
                s = m()
                if s['lvgl_used_pct'] > peak[0]: peak[0] = s['lvgl_used_pct']
                if s['lvgl_free_biggest'] < minbig[0]: minbig[0] = s['lvgl_free_biggest']
                if s['lvgl_used_pct'] >= USED_MAX or s['lvgl_free_biggest'] < BIG_MIN:
                    line('p%d %s STOP' % (p, loc), s); raise Stop()
            try: ss.clear_result_queue(); ss.main_menu_screen({'top_nav': {'title': D['menu_title']}, 'button_list': D['menu_btns']}); t.sleep_ms(40)   # main_menu FIRST, localized
            except Exception: fails += 1
            chk()
            try: blist(title, btn[:8])                                   # short menu
            except Exception: fails += 1
            chk()
            act = btn[:2] if btn else ['OK']
            for i, para in enumerate(paras):                             # paragraph screens
                try:
                    if i % 2 == 0: blist(title, act, text=para)          # paragraph in TEXT field
                    else: status(title, para)                            # paragraph in status body
                except Exception: fails += 1
                chk()
            try: blist(title, D['settings'])                            # full settings: long list
            except Exception: fails += 1
            chk()
            for kbmode in (None, 'upper', 'symbols'):                    # all 3 keyboard pages -> all keyboard glyphs loaded together
                try: passphrase(title, mode=kbmode)
                except Exception: fails += 1
                chk()
            line('p%d %-11s' % (p, loc), m(),
                 'ok=%d para=%-2d peak=%2d%% minbig=%-6d fails=%d' % (ok, len(paras), peak[0], minbig[0], fails))
        torture()
        line('p%d torture' % p, m(), 'scroll+flashing-edges (dwelled)')
    try:
        ss.clear_result_queue(); ss.screensaver_screen(); t.sleep_ms(40)   # 5th type, once at end
    except Exception: pass
    line('screensaver', m())
except Stop:
    pass

ss.unload_locale()
try: blist('Settings', DATA['en']['btn'][:4])
except Exception: pass
line('after unload+en', m())
print('--- done (%d passes x %d locales, 5 screen types + animation torture) ---' % (PASSES, len(LOCS)))
