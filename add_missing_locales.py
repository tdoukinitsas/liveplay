import json
import os
import sys

sys.stdout.reconfigure(encoding='utf-8')

new_strings = {
    'en': {
        'startingLocalServer': 'Starting local server…',
        'serversOnThisNetwork': 'Servers on this network'
    },
    'ar': {
        'startingLocalServer': 'جاري بدء الخادم المحلي…',
        'serversOnThisNetwork': 'الخوادم على هذه الشبكة'
    },
    'bn': {
        'startingLocalServer': 'স্থানীয় সার্ভার চালু করা হচ্ছে…',
        'serversOnThisNetwork': 'এই নেটওয়ার্কে সার্ভার'
    },
    'de': {
        'startingLocalServer': 'Lokaler Server wird gestartet…',
        'serversOnThisNetwork': 'Server in diesem Netzwerk'
    },
    'el': {
        'startingLocalServer': 'Εκκίνηση τοπικού διακομιστή…',
        'serversOnThisNetwork': 'Διακομιστές σε αυτό το δίκτυο'
    },
    'es': {
        'startingLocalServer': 'Iniciando servidor local…',
        'serversOnThisNetwork': 'Servidores en esta red'
    },
    'fa': {
        'startingLocalServer': 'درحال راه‌اندازی سرور محلی…',
        'serversOnThisNetwork': 'سرورها در این شبکه'
    },
    'fr': {
        'startingLocalServer': 'Démarrage du serveur local…',
        'serversOnThisNetwork': 'Serveurs sur ce réseau'
    },
    'hi': {
        'startingLocalServer': 'स्थानीय सर्वर शुरू किया जा रहा है…',
        'serversOnThisNetwork': 'इस नेटवर्क पर सर्वर'
    },
    'it': {
        'startingLocalServer': 'Avvio del server locale…',
        'serversOnThisNetwork': 'Server su questa rete'
    },
    'ja': {
        'startingLocalServer': 'ローカルサーバーを起動中…',
        'serversOnThisNetwork': 'このネットワーク上のサーバー'
    },
    'ko': {
        'startingLocalServer': '로컬 서버 시작 중…',
        'serversOnThisNetwork': '이 네트워크의 서버'
    },
    'no': {
        'startingLocalServer': 'Starter lokal server…',
        'serversOnThisNetwork': 'Servere på dette nettverket'
    },
    'pt': {
        'startingLocalServer': 'Iniciando servidor local…',
        'serversOnThisNetwork': 'Servidores nesta rede'
    },
    'ro': {
        'startingLocalServer': 'Se pornește serverul local…',
        'serversOnThisNetwork': 'Servere pe această rețea'
    },
    'ru': {
        'startingLocalServer': 'Запуск локального сервера…',
        'serversOnThisNetwork': 'Серверы в этой сети'
    },
    'sq': {
        'startingLocalServer': 'Fillimi i serverit lokal…',
        'serversOnThisNetwork': 'Serverët në këtë rrjet'
    },
    'sv': {
        'startingLocalServer': 'Startar lokal server…',
        'serversOnThisNetwork': 'Servrar på detta nätverk'
    },
    'tr': {
        'startingLocalServer': 'Yerel sunucu başlatılıyor…',
        'serversOnThisNetwork': 'Bu ağdaki sunucular'
    },
    'ur': {
        'startingLocalServer': 'مقامی سرور شروع ہو رہا ہے…',
        'serversOnThisNetwork': 'اس نیٹ ورک پر سرورز'
    },
    'zh': {
        'startingLocalServer': '正在启动本地服务器…',
        'serversOnThisNetwork': '该网络上的服务器'
    },
}

locales_dir = 'client/locales'
for code, strings in new_strings.items():
    path = f'{locales_dir}/{code}.json'
    if not os.path.exists(path):
        print(f'MISSING: {path}')
        continue
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    if 'welcome' in data:
        data['welcome'].update(strings)
    else:
        print(f'WARNING: No welcome section in {code}')

    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(f'Updated: {code}')
