<template>
  <div class="language-switcher">
    <button
      type="button"
      class="language-button"
      :aria-label="t('common.changeLanguage')"
      :aria-expanded="isOpen"
      @click="toggleDropdown"
    >
      <span class="material-symbols-rounded" aria-hidden="true" data-icon="language"></span>
      <span class="current-locale">{{ locale }}</span>
    </button>

    <div v-if="isOpen" class="language-dropdown">
      <button
        v-for="(name, code) in locales"
        :key="code"
        type="button"
        class="language-option"
        :class="{ active: locale === code }"
        :lang="code"
        @click="selectLocale(code)"
      >
        {{ name }}
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue';
import { useI18n } from '../composables/useI18n';

const { t, locale, locales, setLocale } = useI18n();
const isOpen = ref(false);

const toggleDropdown = () => {
  isOpen.value = !isOpen.value;
};

const selectLocale = async (code: string) => {
  await setLocale(code);
  isOpen.value = false;
};

// Close dropdown when clicking outside
const handleClickOutside = (event: MouseEvent) => {
  const target = event.target as HTMLElement;
  if (!target.closest('.language-switcher')) {
    isOpen.value = false;
  }
};

const handleKey = (event: KeyboardEvent) => {
  if (event.key === 'Escape') isOpen.value = false;
};

onMounted(() => {
  document.addEventListener('click', handleClickOutside);
  document.addEventListener('keydown', handleKey);
});

onUnmounted(() => {
  document.removeEventListener('click', handleClickOutside);
  document.removeEventListener('keydown', handleKey);
});
</script>

<style scoped lang="scss">
.language-switcher {
  position: relative;

  .language-button {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    min-height: 42px;
    padding: 0 0.7rem;
    background: transparent;
    border: 1px solid var(--line);
    border-radius: 5px;
    color: var(--text);
    cursor: pointer;
    transition: border-color 0.15s ease;

    &:hover {
      border-color: rgba(255, 255, 255, 0.5);
    }

    .material-symbols-rounded {
      font-size: 20px;
    }

    .current-locale {
      text-transform: uppercase;
      font-weight: 600;
      font-size: 0.82rem;
    }
  }

  .language-dropdown {
    position: absolute;
    top: calc(100% + 0.5rem);
    inset-inline-end: 0;
    display: grid;
    grid-template-columns: repeat(2, minmax(120px, 1fr));
    max-height: min(70vh, 560px);
    overflow-y: auto;
    padding: 6px;
    background: var(--ink-3);
    border: 1px solid var(--line);
    border-radius: 6px;
    box-shadow: 0 16px 40px rgba(0, 0, 0, 0.6);
    z-index: 1000;

    .language-option {
      display: block;
      width: 100%;
      padding: 0.55rem 0.8rem;
      background: transparent;
      border: none;
      border-radius: 4px;
      color: var(--text-2);
      cursor: pointer;
      text-align: start;
      font-size: 0.92rem;

      &:hover {
        background: rgba(255, 255, 255, 0.1);
        color: #fff;
      }

      &.active {
        background: var(--go);
        color: #151004;
        font-weight: 700;
      }
    }
  }
}

@media (max-width: 560px) {
  .language-switcher .language-button .current-locale {
    display: none;
  }
}
</style>
