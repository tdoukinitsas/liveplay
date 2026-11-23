import { computed, toValue, type MaybeRefOrGetter } from 'vue';
import seo from '../data/seo.json';
import { useSiteInfo } from './useSiteInfo';

// Search, social and AI metadata for one page. All of it is in English: these
// tags describe the canonical English page, whichever language a visitor reads.
export type PageKey = keyof typeof seo.pages;

interface SeoPage {
  path: string;
  og: string;
  title: string;
  socialTitle: string;
  description: string;
  imageAlt: string;
  published?: string;
}

const { site } = seo;
const WEBSITE_ID = `${site.url}#website`;
const APP_ID = `${site.url}#software`;
const AUTHOR = { '@type': 'Person', name: site.author.name, url: site.author.url };
const LANGUAGES = ['ar', 'bn', 'de', 'el', 'en', 'es', 'fa', 'fr', 'hi', 'it', 'ja', 'ko', 'no', 'pt', 'ro', 'ru', 'sq', 'sv', 'tr', 'ur', 'zh'];

const crumbs = (items: [string, string][]) => ({
  '@type': 'BreadcrumbList',
  itemListElement: items.map(([name, item], i) => ({ '@type': 'ListItem', position: i + 1, name, item })),
});

function schemaFor(key: PageKey, page: SeoPage, version: string, people: { name: string; link: string }[]) {
  const url = site.url + page.path;
  const image = `${site.url}og/${page.og}.jpg`;
  const whatsNew = site.url + seo.pages.whatsNew.path;

  if (key === 'home') {
    return [
      { '@type': 'WebSite', '@id': WEBSITE_ID, url: site.url, name: site.name, description: page.description, inLanguage: 'en' },
      {
        '@type': 'SoftwareApplication',
        '@id': APP_ID,
        name: site.name,
        url: site.url,
        description: site.summary,
        applicationCategory: 'MultimediaApplication',
        applicationSubCategory: 'Sound cue playback',
        operatingSystem: 'Windows 10, Windows 11, macOS 13.3 or later, Linux',
        softwareVersion: version,
        downloadUrl: site.releases,
        releaseNotes: whatsNew,
        screenshot: `${site.url}screenshots/liveplay_screenshot.jpg`,
        image,
        license: site.license,
        isAccessibleForFree: true,
        offers: { '@type': 'Offer', price: '0', priceCurrency: 'USD' },
        featureList: seo.features,
        availableLanguage: LANGUAGES,
        author: AUTHOR,
        contributor: people.map(p => ({ '@type': 'Person', name: p.name, url: p.link })),
        sameAs: [site.repo],
      },
      {
        '@type': 'FAQPage',
        '@id': `${site.url}#faq`,
        url,
        mainEntity: seo.faq.map(({ q, a }) => ({
          '@type': 'Question',
          name: q,
          acceptedAnswer: { '@type': 'Answer', text: a },
        })),
      },
    ];
  }

  if (key === 'whatsNew') {
    return [
      { '@type': 'CollectionPage', '@id': `${url}#page`, url, name: page.title, description: page.description, isPartOf: { '@id': WEBSITE_ID }, about: { '@id': APP_ID }, inLanguage: 'en' },
      crumbs([[site.name, site.url], ["What's New", url]]),
    ];
  }

  return [
    {
      '@type': 'TechArticle',
      '@id': `${url}#article`,
      url,
      mainEntityOfPage: url,
      headline: page.title,
      description: page.description,
      image,
      author: AUTHOR,
      ...(page.published ? { datePublished: page.published } : {}),
      about: { '@type': 'SoftwareApplication', name: site.name, url: site.url, applicationCategory: 'MultimediaApplication', operatingSystem: 'Windows, macOS, Linux' },
      isPartOf: { '@type': 'WebSite', name: site.name, url: site.url },
      inLanguage: 'en',
    },
    crumbs([[site.name, site.url], ["What's New", whatsNew], [page.socialTitle.split(':')[0]!, url]]),
  ];
}

export const usePageSeo = (key: MaybeRefOrGetter<PageKey>) => {
  const { version, contributors } = useSiteInfo();
  const pageKey = computed(() => toValue(key));
  const page = computed<SeoPage>(() => seo.pages[pageKey.value]);
  const url = computed(() => site.url + page.value.path);
  const image = computed(() => `${site.url}og/${page.value.og}.jpg`);

  useHead({
    title: () => page.value.title,
    link: () => [{ rel: 'canonical', href: url.value }],
    script: () => [{
      key: 'structured-data',
      type: 'application/ld+json',
      // Escaped so nothing in the data can close the script tag early.
      innerHTML: JSON.stringify({
        '@context': 'https://schema.org',
        '@graph': schemaFor(pageKey.value, page.value, version.value, [
          { name: site.author.name, link: site.author.url },
          ...contributors.value,
        ]),
      }).replace(/</g, '\\u003c'),
    }],
  });

  useSeoMeta({
    description: () => page.value.description,
    author: site.author.name,
    robots: 'index, follow, max-image-preview:large, max-snippet:-1',
    ogSiteName: site.name,
    ogType: () => (pageKey.value === 'home' || pageKey.value === 'whatsNew' ? 'website' : 'article'),
    ogLocale: 'en_GB',
    ogUrl: url,
    ogTitle: () => page.value.socialTitle,
    ogDescription: () => page.value.description,
    ogImage: image,
    ogImageSecureUrl: image,
    ogImageType: 'image/jpeg',
    ogImageWidth: 1200,
    ogImageHeight: 630,
    ogImageAlt: () => page.value.imageAlt,
    twitterCard: 'summary_large_image',
    twitterTitle: () => page.value.socialTitle,
    twitterDescription: () => page.value.description,
    twitterImage: image,
    twitterImageAlt: () => page.value.imageAlt,
  });
};
