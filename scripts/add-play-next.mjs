import { readFileSync, writeFileSync, readdirSync } from 'fs';
import { join } from 'path';

const PLAY_NEXT = {
  en: 'Play Next',
  de: 'Nächstes abspielen',
  fr: 'Lire le suivant',
  es: 'Reproducir siguiente',
  it: 'Riproduci successivo',
  pt: 'Reproduzir próximo',
  ru: 'Следующий трек',
  ja: '次を再生',
  zh: '播放下一首',
  ko: '다음 재생',
  ar: 'تشغيل التالي',
  tr: 'Sonrakini oynat',
  el: 'Αναπαραγωγή επόμενου',
  no: 'Spill neste',
  sv: 'Spela nästa',
  ro: 'Redare următorul',
  sq: 'Luaj të ardhshmen',
  hi: 'अगला चलाएं',
  bn: 'পরবর্তী চালান',
  fa: 'پخش بعدی',
  ur: 'اگلا چلائیں',
};

const localesDir = './locales';
const files = readdirSync(localesDir).filter(f => f.endsWith('.json'));

for (const file of files) {
  const code = file.replace('.json', '');
  const filePath = join(localesDir, file);
  const data = JSON.parse(readFileSync(filePath, 'utf8'));
  if (data.controls) {
    data.controls.playNext = PLAY_NEXT[code] ?? PLAY_NEXT['en'];
    writeFileSync(filePath, JSON.stringify(data, null, 2) + '\n', 'utf8');
    console.log(`Updated: ${file}`);
  }
}
console.log('Done.');
