import json, os, sys
sys.stdout.reconfigure(encoding='utf-8')

translations = {
    'en': {
        'title': 'Corrupt Project Detected',
        'message': 'This project was found to have issues and has been automatically repaired. The following problems were detected:',
        'dataLossWarning': 'Warning: some data may have been lost during repair.',
        'repairAndSave': 'Repair & Save',
        'openAnyway': 'Open Without Saving',
    },
    'ar': {
        'title': 'تم اكتشاف مشروع تالف',
        'message': 'تم اكتشاف مشكلات في هذا المشروع وتمت إصلاحه تلقائياً. تم اكتشاف المشكلات التالية:',
        'dataLossWarning': 'تحذير: ربما ضاعت بعض البيانات خلال عملية الإصلاح.',
        'repairAndSave': 'إصلاح وحفظ',
        'openAnyway': 'فتح بدون حفظ',
    },
    'bn': {
        'title': 'দূষিত প্রকল্প শনাক্ত হয়েছে',
        'message': 'এই প্রকল্পে সমস্যা পাওয়া গেছে এবং স্বয়ংক্রিয়ভাবে মেরামত করা হয়েছে। নিম্নলিখিত সমস্যাগুলি সনাক্ত হয়েছে:',
        'dataLossWarning': 'সতর্কতা: মেরামতের সময় কিছু ডেটা হারিয়ে যেতে পারে।',
        'repairAndSave': 'মেরামত ও সেভ',
        'openAnyway': 'সেভ না করে খুলুন',
    },
    'de': {
        'title': 'Beschädigtes Projekt erkannt',
        'message': 'Dieses Projekt weist Fehler auf und wurde automatisch repariert. Folgende Probleme wurden festgestellt:',
        'dataLossWarning': 'Warnung: Bei der Reparatur könnten einige Daten verloren gegangen sein.',
        'repairAndSave': 'Reparieren & Speichern',
        'openAnyway': 'Ohne Speichern öffnen',
    },
    'el': {
        'title': 'Εντοπίστηκε κατεστραμμένο έργο',
        'message': 'Αυτό το έργο είχε προβλήματα και επισκευάστηκε αυτόματα. Εντοπίστηκαν τα ακόλουθα προβλήματα:',
        'dataLossWarning': 'Προειδοποίηση: Κάποια δεδομένα ενδέχεται να χαθούν κατά την επισκευή.',
        'repairAndSave': 'Επισκευή και Αποθήκευση',
        'openAnyway': 'Άνοιγμα χωρίς Αποθήκευση',
    },
    'es': {
        'title': 'Proyecto dañado detectado',
        'message': 'Este proyecto tiene problemas y ha sido reparado automáticamente. Se detectaron los siguientes problemas:',
        'dataLossWarning': 'Advertencia: algunos datos pueden haberse perdido durante la reparación.',
        'repairAndSave': 'Reparar y Guardar',
        'openAnyway': 'Abrir sin Guardar',
    },
    'fa': {
        'title': 'پروژه آسیب‌دیده شناسایی شد',
        'message': 'این پروژه دارای مشکل بود و به طور خودکار تعمیر شد. مشکلات زیر شناسایی شدند:',
        'dataLossWarning': 'هشدار: ممکن است برخی داده‌ها در جریان تعمیر از دست رفته باشند.',
        'repairAndSave': 'تعمیر و ذخیره',
        'openAnyway': 'باز کردن بدون ذخیره',
    },
    'fr': {
        'title': 'Projet corrompu détecté',
        'message': 'Ce projet présentait des problèmes et a été réparé automatiquement. Les problèmes suivants ont été détectés :',
        'dataLossWarning': 'Avertissement : des données ont peut-être été perdues lors de la réparation.',
        'repairAndSave': 'Réparer et Enregistrer',
        'openAnyway': 'Ouvrir sans Enregistrer',
    },
    'hi': {
        'title': 'दूषित प्रोजेक्ट मिला',
        'message': 'इस प्रोजेक्ट में समस्याएँ मिलीं और इसे स्वचालित रूप से ठीक किया गया। निम्नलिखित समस्याओं का पता चला:',
        'dataLossWarning': 'चेतावनी: मरम्मत के दौरान कुछ डेटा खो सकता है।',
        'repairAndSave': 'ठीक करें और सहेजें',
        'openAnyway': 'सहेजे बिना खोलें',
    },
    'it': {
        'title': 'Progetto danneggiato rilevato',
        'message': 'Questo progetto presentava problemi ed è stato riparato automaticamente. Sono stati rilevati i seguenti problemi:',
        'dataLossWarning': 'Avviso: alcuni dati potrebbero essere andati persi durante la riparazione.',
        'repairAndSave': 'Ripara e Salva',
        'openAnyway': 'Apri senza Salvare',
    },
    'ja': {
        'title': '破損したプロジェクトを検出',
        'message': 'このプロジェクトに問題が見つかり、自動的に修復されました。次の問題が検出されました：',
        'dataLossWarning': '警告：修復中に一部のデータが失われた可能性があります。',
        'repairAndSave': '修復して保存',
        'openAnyway': '保存せずに開く',
    },
    'ko': {
        'title': '손상된 프로젝트 감지',
        'message': '이 프로젝트에 문제가 발견되어 자동으로 복구되었습니다. 다음 문제가 감지되었습니다:',
        'dataLossWarning': '경고: 복구 과정에서 일부 데이터가 손실되었을 수 있습니다.',
        'repairAndSave': '복구 후 저장',
        'openAnyway': '저장하지 않고 열기',
    },
    'no': {
        'title': 'Skadet prosjekt oppdaget',
        'message': 'Dette prosjektet hadde problemer og er blitt reparert automatisk. Følgende problemer ble oppdaget:',
        'dataLossWarning': 'Advarsel: noen data kan ha gått tapt under reparasjonen.',
        'repairAndSave': 'Reparer og Lagre',
        'openAnyway': 'Åpne uten å Lagre',
    },
    'pt': {
        'title': 'Projeto corrompido detectado',
        'message': 'Este projeto apresentava problemas e foi reparado automaticamente. Os seguintes problemas foram detectados:',
        'dataLossWarning': 'Aviso: alguns dados podem ter sido perdidos durante a reparação.',
        'repairAndSave': 'Reparar e Guardar',
        'openAnyway': 'Abrir sem Guardar',
    },
    'ro': {
        'title': 'Proiect corupt detectat',
        'message': 'Acest proiect a avut probleme și a fost reparat automat. Au fost detectate următoarele probleme:',
        'dataLossWarning': 'Avertisment: unele date ar putea fi pierdute în urma reparării.',
        'repairAndSave': 'Repară și Salvează',
        'openAnyway': 'Deschide fără a Salva',
    },
    'ru': {
        'title': 'Обнаружен повреждённый проект',
        'message': 'В этом проекте обнаружены проблемы, которые были автоматически исправлены. Были обнаружены следующие проблемы:',
        'dataLossWarning': 'Предупреждение: некоторые данные могли быть утеряны в процессе восстановления.',
        'repairAndSave': 'Исправить и Сохранить',
        'openAnyway': 'Открыть без сохранения',
    },
    'sq': {
        'title': 'Projekt i korruptuar i zbuluar',
        'message': 'Ky projekt kishte probleme dhe u riparua automatikisht. U zbuluan problemet e mëposhtme:',
        'dataLossWarning': 'Paralajmërim: disa të dhëna mund të jenë humbur gjatë riparimit.',
        'repairAndSave': 'Riparo dhe Ruaj',
        'openAnyway': 'Hap pa Ruajtur',
    },
    'sv': {
        'title': 'Skadat projekt upptäckt',
        'message': 'Det här projektet hade problem och har reparerats automatiskt. Följande problem hittades:',
        'dataLossWarning': 'Varning: viss data kan ha förlorats vid reparationen.',
        'repairAndSave': 'Reparera och Spara',
        'openAnyway': 'Öppna utan att Spara',
    },
    'tr': {
        'title': 'Bozuk proje tespit edildi',
        'message': 'Bu projede sorunlar tespit edildi ve otomatik olarak onarıldı. Şu sorunlar tespit edildi:',
        'dataLossWarning': 'Uyarı: onarım sırasında bazı veriler kaybolmuş olabilir.',
        'repairAndSave': 'Onar ve Kaydet',
        'openAnyway': 'Kaydetmeden Aç',
    },
    'ur': {
        'title': 'خراب پروجیکٹ ملا',
        'message': 'اس پروجیکٹ میں مسائل پائے گئے اور یہ خودبخود درست کیا گیا۔ مندرجہ ذیل مسائل پائے گئے:',
        'dataLossWarning': 'خبردار: درستگی کے دوران کچھ ڈیٹا ضائع ہو سکتا ہے۔',
        'repairAndSave': 'درست کریں اور سیو کریں',
        'openAnyway': 'سیو کیے بغیر کھولیں',
    },
    'zh': {
        'title': '检测到损坏的项目',
        'message': '此项目存在问题并已被自动修复。检测到以下问题：',
        'dataLossWarning': '警告：修复过程中可能丢失了部分数据。',
        'repairAndSave': '修复并保存',
        'openAnyway': '不保存直接打开',
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
    data['repair'] = trans
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(f'Updated: {code}')
