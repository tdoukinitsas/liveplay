import json
import os
import sys

sys.stdout.reconfigure(encoding='utf-8')

# welcome.serverUnreachable — shown when a server is visible via UDP discovery
# but its TCP control port (4480) can't be reached (typically a firewall on
# the server machine).
new_strings = {
    'en': 'Server found on the network but its connection port is unreachable — check the firewall on the server machine (allow TCP 4480)',
    'ar': 'تم العثور على الخادم على الشبكة لكن منفذ الاتصال الخاص به غير قابل للوصول — تحقق من جدار الحماية على جهاز الخادم (اسمح بـ TCP 4480)',
    'bn': 'নেটওয়ার্কে সার্ভার পাওয়া গেছে কিন্তু এর সংযোগ পোর্টে পৌঁছানো যাচ্ছে না — সার্ভার মেশিনের ফায়ারওয়াল পরীক্ষা করুন (TCP 4480 অনুমতি দিন)',
    'de': 'Server im Netzwerk gefunden, aber sein Verbindungsport ist nicht erreichbar — prüfe die Firewall auf dem Servercomputer (TCP 4480 zulassen)',
    'el': 'Ο διακομιστής βρέθηκε στο δίκτυο αλλά η θύρα σύνδεσής του δεν είναι προσβάσιμη — ελέγξτε το τείχος προστασίας στον υπολογιστή του διακομιστή (επιτρέψτε TCP 4480)',
    'es': 'Servidor encontrado en la red pero su puerto de conexión no es accesible — revisa el firewall del equipo servidor (permite TCP 4480)',
    'fa': 'سرور در شبکه پیدا شد اما پورت اتصال آن در دسترس نیست — فایروال دستگاه سرور را بررسی کنید (TCP 4480 را مجاز کنید)',
    'fr': 'Serveur trouvé sur le réseau mais son port de connexion est inaccessible — vérifiez le pare-feu sur la machine serveur (autorisez le TCP 4480)',
    'hi': 'सर्वर नेटवर्क पर मिला लेकिन उसका कनेक्शन पोर्ट पहुंच से बाहर है — सर्वर मशीन पर फ़ायरवॉल जांचें (TCP 4480 की अनुमति दें)',
    'it': 'Server trovato sulla rete ma la sua porta di connessione non è raggiungibile — controlla il firewall sul computer server (consenti TCP 4480)',
    'ja': 'ネットワーク上にサーバーが見つかりましたが、接続ポートに到達できません — サーバー側のファイアウォールを確認してください（TCP 4480 を許可）',
    'ko': '네트워크에서 서버를 찾았지만 연결 포트에 접근할 수 없습니다 — 서버 컴퓨터의 방화벽을 확인하세요 (TCP 4480 허용)',
    'no': 'Server funnet på nettverket, men tilkoblingsporten er utilgjengelig — sjekk brannmuren på servermaskinen (tillat TCP 4480)',
    'pt': 'Servidor encontrado na rede, mas a porta de conexão está inacessível — verifique o firewall na máquina do servidor (permita TCP 4480)',
    'ro': 'Server găsit în rețea, dar portul său de conexiune este inaccesibil — verifică firewallul de pe mașina server (permite TCP 4480)',
    'ru': 'Сервер найден в сети, но его порт подключения недоступен — проверьте брандмауэр на сервере (разрешите TCP 4480)',
    'sq': 'Serveri u gjet në rrjet, por porta e tij e lidhjes është e paarritshme — kontrollo murin e zjarrit në makinën e serverit (lejo TCP 4480)',
    'sv': 'Servern hittades på nätverket men dess anslutningsport är inte nåbar — kontrollera brandväggen på serverdatorn (tillåt TCP 4480)',
    'tr': 'Sunucu ağda bulundu ancak bağlantı bağlantı noktasına erişilemiyor — sunucu makinesindeki güvenlik duvarını kontrol edin (TCP 4480 izni verin)',
    'ur': 'سرور نیٹ ورک پر مل گیا لیکن اس کا کنکشن پورٹ ناقابل رسائی ہے — سرور مشین پر فائر وال چیک کریں (TCP 4480 کی اجازت دیں)',
    'zh': '已在网络中找到服务器，但其连接端口无法访问 — 请检查服务器计算机上的防火墙（允许 TCP 4480）',
}

locales_dir = 'client/locales'
for code, value in new_strings.items():
    path = f'{locales_dir}/{code}.json'
    if not os.path.exists(path):
        print(f'MISSING: {path}')
        continue
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    if 'welcome' in data:
        data['welcome']['serverUnreachable'] = value
    else:
        print(f'WARNING: No welcome section in {code}')
        continue

    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(f'Updated: {code}')
