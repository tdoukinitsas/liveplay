import json
import os
import sys

sys.stdout.reconfigure(encoding='utf-8')

translations = {
    'en': {
        'about': {
            'license': 'AGPL-3.0-only License'
        },
        'connectionLost': {
            'title': 'Connection lost',
            'message': 'The connection to the LivePlay server at {url} has been lost.',
            'attempting': 'Attempting to reconnect…',
            'reconnect': 'Reconnect',
            'restart': 'Restart',
            'exit': 'Exit',
            'reconnectHint': 'Reconnect tries the same server again.',
            'restartHint': 'Restart relaunches just the client (the audio server keeps running).',
            'exitHint': 'Exit quits the client.'
        },
        'audioImport': {
            'title': 'Import audio',
            'tabServer': 'On server',
            'tabUpload': 'Upload from this computer',
            'serverHint': 'Files already on the server. Double-click a folder to descend, or click Add next to a file.',
            'uploadDescription': 'Pick one or more files from this computer. They\'ll be uploaded into the server\'s media folder.',
            'chooseFiles': 'Choose files…',
            'uploading': 'Uploading…',
            'uploadingProgress': 'Uploading {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Restart server'
        },
        'serverSettings': {
            'reconnecting': '● Reconnecting…',
            'restartEngine': 'Restart engine'
        },
    },
    'ar': {
        'about': {
            'license': 'رخصة AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'فُقِد الاتصال',
            'message': 'فُقِد الاتصال بخادم LivePlay في {url}.',
            'attempting': 'محاولة إعادة الاتصال…',
            'reconnect': 'إعادة الاتصال',
            'restart': 'إعادة تشغيل',
            'exit': 'خروج',
            'reconnectHint': 'تحاول إعادة الاتصال بنفس الخادم.',
            'restartHint': 'إعادة التشغيل تعيد تشغيل العميل فقط (يستمر خادم الصوت في التشغيل).',
            'exitHint': 'الخروج يغلق العميل.'
        },
        'audioImport': {
            'title': 'استيراد الصوت',
            'tabServer': 'على الخادم',
            'tabUpload': 'رفع من هذا الكمبيوتر',
            'serverHint': 'الملفات الموجودة على الخادم بالفعل. انقر نقراً مزدوجاً على مجلد للنزول، أو انقر إضافة بجانب ملف.',
            'uploadDescription': 'اختر ملفاً واحداً أو أكثر من هذا الكمبيوتر. سيتم رفعها إلى مجلد الوسائط على الخادم.',
            'chooseFiles': 'اختر الملفات…',
            'uploading': 'جاري الرفع…',
            'uploadingProgress': 'جاري الرفع {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'إعادة تشغيل الخادم'
        },
        'serverSettings': {
            'reconnecting': '● إعادة الاتصال…',
            'restartEngine': 'إعادة تشغيل المحرك'
        },
    },
    'bn': {
        'about': {
            'license': 'AGPL-3.0-only লাইসেন্স'
        },
        'connectionLost': {
            'title': 'সংযোগ হারিয়েছে',
            'message': '{url} তে LivePlay সার্ভারের সংযোগ হারিয়ে গেছে।',
            'attempting': 'পুনরায় সংযোগ করার চেষ্টা করা হচ্ছে…',
            'reconnect': 'পুনরায় সংযোগ করুন',
            'restart': 'পুনরায় শুরু করুন',
            'exit': 'বেরিয়ে যান',
            'reconnectHint': 'পুনরায় সংযোগ একই সার্ভার আবার চেষ্টা করে।',
            'restartHint': 'পুনরায় শুরু করুন শুধুমাত্র ক্লায়েন্ট পুনরায় লঞ্চ করে (অডিও সার্ভার চলতে থাকে)।',
            'exitHint': 'বেরিয়ে যান ক্লায়েন্ট বন্ধ করে।'
        },
        'audioImport': {
            'title': 'অডিও আমদানি করুন',
            'tabServer': 'সার্ভারে',
            'tabUpload': 'এই কম্পিউটার থেকে আপলোড করুন',
            'serverHint': 'ইতিমধ্যে সার্ভারে থাকা ফাইলগুলি। একটি ফোল্ডার নেমে যেতে দ্বিগুণ ক্লিক করুন, বা একটি ফাইলের পাশে যোগ করুন ক্লিক করুন।',
            'uploadDescription': 'এই কম্পিউটার থেকে এক বা একাধিক ফাইল চয়ন করুন। তারা সার্ভারের মিডিয়া ফোল্ডারে আপলোড করা হবে।',
            'chooseFiles': 'ফাইলগুলি চয়ন করুন…',
            'uploading': 'আপলোড করা হচ্ছে…',
            'uploadingProgress': 'আপলোড করা হচ্ছে {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'সার্ভার পুনরায় শুরু করুন'
        },
        'serverSettings': {
            'reconnecting': '● পুনরায় সংযোগ করা হচ্ছে…',
            'restartEngine': 'ইঞ্জিন পুনরায় শুরু করুন'
        },
    },
    'de': {
        'about': {
            'license': 'AGPL-3.0-only Lizenz'
        },
        'connectionLost': {
            'title': 'Verbindung unterbrochen',
            'message': 'Die Verbindung zum LivePlay-Server bei {url} wurde unterbrochen.',
            'attempting': 'Versuche, Verbindung wiederherzustellen…',
            'reconnect': 'Erneut verbinden',
            'restart': 'Neustart',
            'exit': 'Beenden',
            'reconnectHint': 'Erneut verbinden versucht, sich mit demselben Server zu verbinden.',
            'restartHint': 'Neustart startet nur den Client neu (der Audio-Server läuft weiter).',
            'exitHint': 'Beenden schließt den Client.'
        },
        'audioImport': {
            'title': 'Audio importieren',
            'tabServer': 'Auf Server',
            'tabUpload': 'Von diesem Computer hochladen',
            'serverHint': 'Dateien, die bereits auf dem Server sind. Doppelklicken Sie auf einen Ordner zum Navigieren oder klicken Sie auf Hinzufügen neben einer Datei.',
            'uploadDescription': 'Wählen Sie eine oder mehrere Dateien von diesem Computer aus. Sie werden in den Medienvordner des Servers hochgeladen.',
            'chooseFiles': 'Dateien auswählen…',
            'uploading': 'Wird hochgeladen…',
            'uploadingProgress': 'Wird hochgeladen {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Server neu starten'
        },
        'serverSettings': {
            'reconnecting': '● Wird erneut verbunden…',
            'restartEngine': 'Engine neu starten'
        },
    },
    'el': {
        'about': {
            'license': 'Άδεια AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Η σύνδεση χάθηκε',
            'message': 'Η σύνδεση με το διακομιστή LivePlay στο {url} έχει χαθεί.',
            'attempting': 'Προσπάθεια επανασύνδεσης…',
            'reconnect': 'Επανασύνδεση',
            'restart': 'Επανεκκίνηση',
            'exit': 'Έξοδος',
            'reconnectHint': 'Η επανασύνδεση προσπαθεί να συνδεθεί ξανά με τον ίδιο διακομιστή.',
            'restartHint': 'Η επανεκκίνηση επανεκκινεί μόνο τον πελάτη (ο διακομιστής ήχου συνεχίζει να εκτελείται).',
            'exitHint': 'Η έξοδος κλείνει τον πελάτη.'
        },
        'audioImport': {
            'title': 'Εισαγωγή ήχου',
            'tabServer': 'Στο διακομιστή',
            'tabUpload': 'Μεταφόρτωση από αυτόν τον υπολογιστή',
            'serverHint': 'Αρχεία που υπάρχουν ήδη στον διακομιστή. Κάντε διπλό κλικ σε έναν φάκελο για να κατέβετε ή κάντε κλικ στο Προσθήκη δίπλα σε ένα αρχείο.',
            'uploadDescription': 'Επιλέξτε ένα ή περισσότερα αρχεία από αυτόν τον υπολογιστή. Θα μεταφορτωθούν στο φάκελο πολυμέσων του διακομιστή.',
            'chooseFiles': 'Επιλέξτε αρχεία…',
            'uploading': 'Μεταφόρτωση…',
            'uploadingProgress': 'Μεταφόρτωση {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Επανεκκίνηση διακομιστή'
        },
        'serverSettings': {
            'reconnecting': '● Επανασύνδεση…',
            'restartEngine': 'Επανεκκίνηση κινητήρα'
        },
    },
    'es': {
        'about': {
            'license': 'Licencia AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Se perdió la conexión',
            'message': 'Se perdió la conexión con el servidor de LivePlay en {url}.',
            'attempting': 'Intentando reconectar…',
            'reconnect': 'Reconectar',
            'restart': 'Reiniciar',
            'exit': 'Salir',
            'reconnectHint': 'Reconectar intenta el mismo servidor de nuevo.',
            'restartHint': 'Reiniciar relanza solo el cliente (el servidor de audio sigue ejecutándose).',
            'exitHint': 'Salir cierra el cliente.'
        },
        'audioImport': {
            'title': 'Importar audio',
            'tabServer': 'En servidor',
            'tabUpload': 'Cargar desde este ordenador',
            'serverHint': 'Archivos ya en el servidor. Haga doble clic en una carpeta para descender o haga clic en Añadir junto a un archivo.',
            'uploadDescription': 'Seleccione uno o más archivos de este ordenador. Se cargarán en la carpeta de medios del servidor.',
            'chooseFiles': 'Elegir archivos…',
            'uploading': 'Cargando…',
            'uploadingProgress': 'Cargando {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Reiniciar servidor'
        },
        'serverSettings': {
            'reconnecting': '● Reconectando…',
            'restartEngine': 'Reiniciar motor'
        },
    },
    'fa': {
        'about': {
            'license': 'مجوز AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'اتصال قطع شد',
            'message': 'اتصال به سرور LivePlay در {url} قطع شده است.',
            'attempting': 'تلاش برای اتصال مجدد…',
            'reconnect': 'اتصال مجدد',
            'restart': 'راه‌اندازی مجدد',
            'exit': 'خروج',
            'reconnectHint': 'اتصال مجدد همان سرور را دوباره سعی می‌کند.',
            'restartHint': 'راه‌اندازی مجدد فقط کلاینت را راه‌اندازی می‌کند (سرور صوتی همچنان اجرا می‌شود).',
            'exitHint': 'خروج کلاینت را بسته می‌کند.'
        },
        'audioImport': {
            'title': 'وارد کردن صوت',
            'tabServer': 'روی سرور',
            'tabUpload': 'آپلود از این کامپیوتر',
            'serverHint': 'فایل‌هایی که قبلاً روی سرور هستند. برای فرود، روی پوشه دوبار کلیک کنید یا روی اضافه کردن کنار یک فایل کلیک کنید.',
            'uploadDescription': 'یک یا چند فایل از این کامپیوتر انتخاب کنید. آن‌ها به پوشه رسانه سرور آپلود خواهند شد.',
            'chooseFiles': 'انتخاب فایل‌ها…',
            'uploading': 'در حال آپلود…',
            'uploadingProgress': 'در حال آپلود {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'راه‌اندازی مجدد سرور'
        },
        'serverSettings': {
            'reconnecting': '● در حال اتصال مجدد…',
            'restartEngine': 'راه‌اندازی مجدد موتور'
        },
    },
    'fr': {
        'about': {
            'license': 'Licence AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Connexion perdue',
            'message': 'La connexion au serveur LivePlay à {url} a été perdue.',
            'attempting': 'Tentative de reconnexion…',
            'reconnect': 'Reconnecter',
            'restart': 'Redémarrer',
            'exit': 'Quitter',
            'reconnectHint': 'Reconnecter essaie le même serveur à nouveau.',
            'restartHint': 'Redémarrer relance uniquement le client (le serveur audio continue de s\'exécuter).',
            'exitHint': 'Quitter ferme le client.'
        },
        'audioImport': {
            'title': 'Importer l\'audio',
            'tabServer': 'Sur serveur',
            'tabUpload': 'Télécharger à partir de cet ordinateur',
            'serverHint': 'Fichiers déjà sur le serveur. Double-cliquez sur un dossier pour descendre ou cliquez sur Ajouter à côté d\'un fichier.',
            'uploadDescription': 'Choisissez un ou plusieurs fichiers sur cet ordinateur. Ils seront téléchargés dans le dossier médias du serveur.',
            'chooseFiles': 'Choisir les fichiers…',
            'uploading': 'Téléchargement…',
            'uploadingProgress': 'Téléchargement {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Redémarrer le serveur'
        },
        'serverSettings': {
            'reconnecting': '● Reconnexion…',
            'restartEngine': 'Redémarrer le moteur'
        },
    },
    'hi': {
        'about': {
            'license': 'AGPL-3.0-only लाइसेंस'
        },
        'connectionLost': {
            'title': 'कनेक्शन खो गया',
            'message': '{url} पर LivePlay सर्वर का कनेक्शन खो गया है।',
            'attempting': 'पुनः कनेक्ट करने का प्रयास किया जा रहा है…',
            'reconnect': 'पुनः कनेक्ट करें',
            'restart': 'पुनः शुरू करें',
            'exit': 'बाहर निकलें',
            'reconnectHint': 'पुनः कनेक्ट करना एक ही सर्वर को फिर से प्रयास करता है।',
            'restartHint': 'पुनः शुरू करना केवल क्लाइंट को पुनः लॉन्च करता है (ऑडियो सर्वर चलता रहता है)।',
            'exitHint': 'बाहर निकलना क्लाइंट को बंद करता है।'
        },
        'audioImport': {
            'title': 'ऑडियो आयात करें',
            'tabServer': 'सर्वर पर',
            'tabUpload': 'इस कंप्यूटर से अपलोड करें',
            'serverHint': 'सर्वर पर पहले से मौजूद फ़ाइलें। एक फ़ोल्डर में उतरने के लिए डबल-क्लिक करें या किसी फ़ाइल के आगे जोड़ें पर क्लिक करें।',
            'uploadDescription': 'इस कंप्यूटर से एक या अधिक फ़ाइलें चुनें। उन्हें सर्वर के मीडिया फ़ोल्डर में अपलोड किया जाएगा।',
            'chooseFiles': 'फ़ाइलें चुनें…',
            'uploading': 'अपलोड किया जा रहा है…',
            'uploadingProgress': 'अपलोड किया जा रहा है {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'सर्वर को पुनः शुरू करें'
        },
        'serverSettings': {
            'reconnecting': '● पुनः कनेक्ट किया जा रहा है…',
            'restartEngine': 'इंजन को पुनः शुरू करें'
        },
    },
    'it': {
        'about': {
            'license': 'Licenza AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Connessione persa',
            'message': 'La connessione al server LivePlay su {url} è stata persa.',
            'attempting': 'Tentativo di riconnessione…',
            'reconnect': 'Riconnetti',
            'restart': 'Riavvia',
            'exit': 'Esci',
            'reconnectHint': 'Riconnetti tenta lo stesso server di nuovo.',
            'restartHint': 'Riavvia relancia solo il client (il server audio continua a funzionare).',
            'exitHint': 'Esci chiude il client.'
        },
        'audioImport': {
            'title': 'Importa audio',
            'tabServer': 'Su server',
            'tabUpload': 'Carica da questo computer',
            'serverHint': 'File già sul server. Fai doppio clic su una cartella per scendere o fai clic su Aggiungi accanto a un file.',
            'uploadDescription': 'Seleziona uno o più file da questo computer. Saranno caricati nella cartella multimediale del server.',
            'chooseFiles': 'Scegli i file…',
            'uploading': 'Caricamento…',
            'uploadingProgress': 'Caricamento {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Riavvia server'
        },
        'serverSettings': {
            'reconnecting': '● Riconnessione…',
            'restartEngine': 'Riavvia motore'
        },
    },
    'ja': {
        'about': {
            'license': 'AGPL-3.0-only ライセンス'
        },
        'connectionLost': {
            'title': '接続が失われました',
            'message': '{url} の LivePlay サーバーへの接続が失われました。',
            'attempting': '再接続を試みています…',
            'reconnect': '再接続',
            'restart': '再起動',
            'exit': '終了',
            'reconnectHint': '再接続は同じサーバーに再度接続しようとします。',
            'restartHint': '再起動はクライアントのみを再起動します（オーディオサーバーは実行を継続します）。',
            'exitHint': '終了はクライアントを閉じます。'
        },
        'audioImport': {
            'title': 'オーディオをインポート',
            'tabServer': 'サーバー上',
            'tabUpload': 'このコンピュータからアップロード',
            'serverHint': 'すでにサーバーにあるファイル。フォルダをダブルクリックして下降するか、ファイルの横にある「追加」をクリックします。',
            'uploadDescription': 'このコンピュータから 1 つ以上のファイルを選択します。サーバーのメディア フォルダーにアップロードされます。',
            'chooseFiles': 'ファイルを選択…',
            'uploading': 'アップロード中…',
            'uploadingProgress': 'アップロード中 {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'サーバーを再起動'
        },
        'serverSettings': {
            'reconnecting': '● 再接続中…',
            'restartEngine': 'エンジンを再起動'
        },
    },
    'ko': {
        'about': {
            'license': 'AGPL-3.0-only 라이센스'
        },
        'connectionLost': {
            'title': '연결이 끊어졌습니다',
            'message': '{url}의 LivePlay 서버에 대한 연결이 끊어졌습니다.',
            'attempting': '다시 연결을 시도 중…',
            'reconnect': '다시 연결',
            'restart': '다시 시작',
            'exit': '종료',
            'reconnectHint': '다시 연결하면 같은 서버를 다시 시도합니다.',
            'restartHint': '다시 시작하면 클라이언트만 다시 실행됩니다(오디오 서버는 계속 실행됨).',
            'exitHint': '종료하면 클라이언트가 닫힙니다.'
        },
        'audioImport': {
            'title': '오디오 가져오기',
            'tabServer': '서버에서',
            'tabUpload': '이 컴퓨터에서 업로드',
            'serverHint': '서버에 이미 있는 파일입니다. 폴더를 두 번 클릭하여 내려가거나 파일 옆의 추가를 클릭합니다.',
            'uploadDescription': '이 컴퓨터에서 하나 이상의 파일을 선택합니다. 서버의 미디어 폴더에 업로드됩니다.',
            'chooseFiles': '파일 선택…',
            'uploading': '업로드 중…',
            'uploadingProgress': '업로드 중 {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': '서버 다시 시작'
        },
        'serverSettings': {
            'reconnecting': '● 다시 연결 중…',
            'restartEngine': '엔진 다시 시작'
        },
    },
    'no': {
        'about': {
            'license': 'AGPL-3.0-only lisens'
        },
        'connectionLost': {
            'title': 'Forbindelse mistet',
            'message': 'Forbindelsen til LivePlay-serveren på {url} er mistet.',
            'attempting': 'Forsøker å koble til igjen…',
            'reconnect': 'Koble til igjen',
            'restart': 'Start på nytt',
            'exit': 'Avslutt',
            'reconnectHint': 'Koble til igjen forsøker samme server igjen.',
            'restartHint': 'Start på nytt relanserer bare klienten (lydserveren fortsetter å kjøre).',
            'exitHint': 'Avslutt lukker klienten.'
        },
        'audioImport': {
            'title': 'Importer lyd',
            'tabServer': 'På server',
            'tabUpload': 'Last opp fra denne datamaskinen',
            'serverHint': 'Filer som allerede er på serveren. Dobbeltklikk på en mappe for å gå ned, eller klikk Legg til ved siden av en fil.',
            'uploadDescription': 'Velg en eller flere filer fra denne datamaskinen. De blir lastet opp til servermediamappen.',
            'chooseFiles': 'Velg filer…',
            'uploading': 'Laster opp…',
            'uploadingProgress': 'Laster opp {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Start server på nytt'
        },
        'serverSettings': {
            'reconnecting': '● Kobler til igjen…',
            'restartEngine': 'Start motor på nytt'
        },
    },
    'pt': {
        'about': {
            'license': 'Licença AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Conexão perdida',
            'message': 'A conexão com o servidor LivePlay em {url} foi perdida.',
            'attempting': 'Tentando reconectar…',
            'reconnect': 'Reconectar',
            'restart': 'Reiniciar',
            'exit': 'Sair',
            'reconnectHint': 'Reconectar tenta o mesmo servidor novamente.',
            'restartHint': 'Reiniciar relança apenas o cliente (o servidor de áudio continua em execução).',
            'exitHint': 'Sair fecha o cliente.'
        },
        'audioImport': {
            'title': 'Importar áudio',
            'tabServer': 'No servidor',
            'tabUpload': 'Enviar deste computador',
            'serverHint': 'Arquivos já no servidor. Clique duas vezes em uma pasta para descer ou clique em Adicionar ao lado de um arquivo.',
            'uploadDescription': 'Selecione um ou mais arquivos neste computador. Eles serão carregados na pasta de mídia do servidor.',
            'chooseFiles': 'Escolher arquivos…',
            'uploading': 'Enviando…',
            'uploadingProgress': 'Enviando {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Reiniciar servidor'
        },
        'serverSettings': {
            'reconnecting': '● Reconectando…',
            'restartEngine': 'Reiniciar motor'
        },
    },
    'ro': {
        'about': {
            'license': 'Licență AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Conexiune pierdută',
            'message': 'Conexiunea cu serverul LivePlay la {url} a fost pierdută.',
            'attempting': 'Se încearcă reconectarea…',
            'reconnect': 'Reconectare',
            'restart': 'Redeschidere',
            'exit': 'Ieşire',
            'reconnectHint': 'Reconectarea încearcă din nou același server.',
            'restartHint': 'Redeschiderea relansează doar clientul (serverul audio continuă să funcționeze).',
            'exitHint': 'Ieşirea închide clientul.'
        },
        'audioImport': {
            'title': 'Importare audio',
            'tabServer': 'Pe server',
            'tabUpload': 'Încărcați de pe acest calculator',
            'serverHint': 'Fișiere deja pe server. Faceți dublu clic pe un folder pentru a coborî sau faceți clic pe Adăugare lângă un fișier.',
            'uploadDescription': 'Selectați uno o mai mulți fișiere de pe acest calculator. Vor fi încărcate în dosarul de media al serverului.',
            'chooseFiles': 'Alegeți fișiere…',
            'uploading': 'Se încarcă…',
            'uploadingProgress': 'Se încarcă {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Redeschidere server'
        },
        'serverSettings': {
            'reconnecting': '● Se reconectează…',
            'restartEngine': 'Redeschidere motor'
        },
    },
    'ru': {
        'about': {
            'license': 'Лицензия AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Соединение потеряно',
            'message': 'Соединение с сервером LivePlay на {url} потеряно.',
            'attempting': 'Попытка переподключения…',
            'reconnect': 'Переподключиться',
            'restart': 'Перезагрузить',
            'exit': 'Выход',
            'reconnectHint': 'Переподключение повторно пытается подключиться к тому же серверу.',
            'restartHint': 'Перезагрузка перезапускает только клиента (сервер звука продолжает работать).',
            'exitHint': 'Выход закрывает клиента.'
        },
        'audioImport': {
            'title': 'Импорт аудио',
            'tabServer': 'На сервере',
            'tabUpload': 'Загрузить с этого компьютера',
            'serverHint': 'Файлы, уже находящиеся на сервере. Дважды щелкните на папку для навигации или щелкните на кнопку «Добавить» рядом с файлом.',
            'uploadDescription': 'Выберите один или несколько файлов с этого компьютера. Они будут загружены в папку медиа сервера.',
            'chooseFiles': 'Выбрать файлы…',
            'uploading': 'Загружаю…',
            'uploadingProgress': 'Загружаю {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Перезагрузить сервер'
        },
        'serverSettings': {
            'reconnecting': '● Переподключение…',
            'restartEngine': 'Перезагрузить движок'
        },
    },
    'sq': {
        'about': {
            'license': 'Licenca AGPL-3.0-only'
        },
        'connectionLost': {
            'title': 'Lidhja u humb',
            'message': 'Lidhja me serverin LivePlay në {url} u humb.',
            'attempting': 'Duke u përpjekur të ribindehet…',
            'reconnect': 'Ribindehu',
            'restart': 'Rifillo',
            'exit': 'Dilni',
            'reconnectHint': 'Ribindehu përpiqet të njëjtin server përsëri.',
            'restartHint': 'Rifillo relanshin vetëm klientin (serveri i zërit vazhdon të funksionojë).',
            'exitHint': 'Dilni mbyll klientin.'
        },
        'audioImport': {
            'title': 'Importo zërin',
            'tabServer': 'Në server',
            'tabUpload': 'Ngarko nga ky kompjuter',
            'serverHint': 'Skedarë tashmë në server. Klikoni dy herë në një dosje për të zbritur ose klikoni në Shto pranë një skede.',
            'uploadDescription': 'Zgjidhni një ose më shumë skeda nga ky kompjuter. Ato do të ngarkohen në dosjen e mediave të serverit.',
            'chooseFiles': 'Zgjedhni skedarë…',
            'uploading': 'Ngarkim…',
            'uploadingProgress': 'Ngarkim {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Rifillo serverin'
        },
        'serverSettings': {
            'reconnecting': '● Ribidhje…',
            'restartEngine': 'Rifillo motorin'
        },
    },
    'sv': {
        'about': {
            'license': 'AGPL-3.0-only-licens'
        },
        'connectionLost': {
            'title': 'Anslutningen förlorades',
            'message': 'Anslutningen till LivePlay-servern på {url} gick förlorad.',
            'attempting': 'Försöker återansluta…',
            'reconnect': 'Återanslut',
            'restart': 'Starta om',
            'exit': 'Avsluta',
            'reconnectHint': 'Återanslut försöker samma server igen.',
            'restartHint': 'Starta om startar bara om klienten (ljudservern fortsätter att köras).',
            'exitHint': 'Avsluta stänger klienten.'
        },
        'audioImport': {
            'title': 'Importera ljud',
            'tabServer': 'På servern',
            'tabUpload': 'Ladda upp från denna dator',
            'serverHint': 'Filer redan på servern. Dubbelklicka på en mapp för att gå ned eller klicka på Lägg till bredvid en fil.',
            'uploadDescription': 'Välj en eller flera filer från denna dator. De laddas upp till serverns mediamapp.',
            'chooseFiles': 'Välj filer…',
            'uploading': 'Laddar upp…',
            'uploadingProgress': 'Laddar upp {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Starta om servern'
        },
        'serverSettings': {
            'reconnecting': '● Återansluter…',
            'restartEngine': 'Starta om motorn'
        },
    },
    'tr': {
        'about': {
            'license': 'AGPL-3.0-only Lisansı'
        },
        'connectionLost': {
            'title': 'Bağlantı kaybedildi',
            'message': '{url} adresindeki LivePlay sunucusuna olan bağlantı kaybedildi.',
            'attempting': 'Yeniden bağlanılmaya çalışılıyor…',
            'reconnect': 'Yeniden bağlan',
            'restart': 'Yeniden başlat',
            'exit': 'Çık',
            'reconnectHint': 'Yeniden bağlan aynı sunucuya tekrar bağlanmayı dener.',
            'restartHint': 'Yeniden başlat yalnızca istemciyi yeniden başlatır (ses sunucusu çalışmaya devam eder).',
            'exitHint': 'Çık istemciyi kapatır.'
        },
        'audioImport': {
            'title': 'Sesi İçe Aktar',
            'tabServer': 'Sunucuda',
            'tabUpload': 'Bu bilgisayardan yükle',
            'serverHint': 'Sunucuda zaten olan dosyalar. İnmek için bir klasöre çift tıklayın veya bir dosyayı yanındaki Ekle\'ye tıklayın.',
            'uploadDescription': 'Bu bilgisayardan bir veya daha fazla dosya seçin. Sunucunun medya klasörüne yüklenir.',
            'chooseFiles': 'Dosyaları seç…',
            'uploading': 'Yükleniyor…',
            'uploadingProgress': 'Yükleniyor {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'Sunucuyu yeniden başlat'
        },
        'serverSettings': {
            'reconnecting': '● Yeniden bağlanılıyor…',
            'restartEngine': 'Motoru yeniden başlat'
        },
    },
    'ur': {
        'about': {
            'license': 'AGPL-3.0-only لائسنس'
        },
        'connectionLost': {
            'title': 'کنکشن ٹوٹ گیا',
            'message': '{url} پر LivePlay سرور سے کنکشن ٹوٹ گیا۔',
            'attempting': 'دوبارہ کنکٹ کرنے کی کوشش کی جا رہی ہے…',
            'reconnect': 'دوبارہ کنکٹ کریں',
            'restart': 'دوبارہ شروع کریں',
            'exit': 'نکلیں',
            'reconnectHint': 'دوبارہ کنکٹ کرنا ایک جیسے سرور کو دوبارہ کوشش کرتا ہے۔',
            'restartHint': 'دوبارہ شروع کرنا صرف کلائنٹ کو دوبارہ شروع کرتا ہے (آڈیو سرور چلتا رہتا ہے)۔',
            'exitHint': 'نکلنا کلائنٹ کو بند کرتا ہے۔'
        },
        'audioImport': {
            'title': 'آڈیو درآمد کریں',
            'tabServer': 'سرور پر',
            'tabUpload': 'اس کمپیوٹر سے اپ لوڈ کریں',
            'serverHint': 'فائلیں جو پہلے سے سرور پر ہیں۔ نیچے جانے کے لیے فولڈر پر ڈبل کلک کریں یا فائل کے ساتھ شامل کریں کو کلک کریں۔',
            'uploadDescription': 'اس کمپیوٹر سے ایک یا زیادہ فائلیں منتخب کریں۔ یہ سرور کے میڈیا فولڈر میں اپ لوڈ کریں گی۔',
            'chooseFiles': 'فائلیں منتخب کریں…',
            'uploading': 'اپ لوڈ ہو رہا ہے…',
            'uploadingProgress': 'اپ لوڈ ہو رہا ہے {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': 'سرور دوبارہ شروع کریں'
        },
        'serverSettings': {
            'reconnecting': '● دوبارہ کنکٹ ہو رہا ہے…',
            'restartEngine': 'انجن دوبارہ شروع کریں'
        },
    },
    'zh': {
        'about': {
            'license': 'AGPL-3.0-only 许可'
        },
        'connectionLost': {
            'title': '连接丢失',
            'message': '与 {url} 上的 LivePlay 服务器的连接已丢失。',
            'attempting': '正在尝试重新连接…',
            'reconnect': '重新连接',
            'restart': '重新启动',
            'exit': '退出',
            'reconnectHint': '重新连接尝试再次连接同一服务器。',
            'restartHint': '重新启动仅重新启动客户端（音频服务器继续运行）。',
            'exitHint': '退出关闭客户端。'
        },
        'audioImport': {
            'title': '导入音频',
            'tabServer': '在服务器上',
            'tabUpload': '从此计算机上传',
            'serverHint': '已在服务器上的文件。双击文件夹以下降，或单击文件旁的添加。',
            'uploadDescription': '从此计算机中选择一个或多个文件。它们将上传到服务器的媒体文件夹。',
            'chooseFiles': '选择文件…',
            'uploading': '正在上传…',
            'uploadingProgress': '正在上传 {current}/{total}: {filename}'
        },
        'localServer': {
            'restartServer': '重新启动服务器'
        },
        'serverSettings': {
            'reconnecting': '● 正在重新连接…',
            'restartEngine': '重新启动引擎'
        },
    },
}

locales_dir = 'client/locales'
for code, trans in translations.items():
    path = f'{locales_dir}/{code}.json'
    if not os.path.exists(path):
        print(f'MISSING: {path}')
        continue
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # Add or update each translation section
    for section, strings in trans.items():
        if section not in data:
            data[section] = {}
        data[section].update(strings)

    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(f'Updated: {code}')
