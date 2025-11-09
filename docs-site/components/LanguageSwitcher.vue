<template>
  <div class="language-switcher">
    <button 
      class="language-button"
      @click="toggleDropdown"
      :aria-label="'Change language'"
    >
      <span class="material-symbols-rounded">language</span>
      <span class="current-locale">{{ locale }}</span>
    </button>
    
    <div v-if="isOpen" class="language-dropdown">
      <button
        v-for="(name, code) in locales"
        :key="code"
        class="language-option"
        :class="{ active: locale === code }"
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

const { locale, locales, setLocale } = useI18n();
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

onMounted(() => {
  document.addEventListener('click', handleClickOutside);
});

onUnmounted(() => {
  document.removeEventListener('click', handleClickOutside);
});
</script>

<style scoped lang="scss">
.language-switcher {
  position: relative;

  .language-button {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.75rem 1rem;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 8px;
    color: white;
    cursor: pointer;
    transition: all 0.2s ease;
    font-size: 1rem;

    &:hover {
      background: rgba(255, 255, 255, 0.1);
      border-color: #DA1E28;
    }

    .material-symbols-rounded {
      font-size: 20px;
    }

    .current-locale {
      text-transform: uppercase;
      font-weight: 600;
      font-size: 0.875rem;
    }
  }

  .language-dropdown {
    position: absolute;
    top: calc(100% + 0.5rem);
    right: 0;
    background: rgba(30, 30, 30, 0.98);
    border: 1px solid rgba(255, 255, 255, 0.1);
    border-radius: 8px;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.5);
    min-width: 180px;
    overflow: hidden;
    z-index: 1000;
    backdrop-filter: blur(10px);

    .language-option {
      display: block;
      width: 100%;
      padding: 0.875rem 1.25rem;
      background: transparent;
      border: none;
      color: rgba(255, 255, 255, 0.85);
      cursor: pointer;
      transition: all 0.2s ease;
      text-align: left;
      font-size: 0.9375rem;

      &:hover {
        background: rgba(255, 255, 255, 0.1);
        color: white;
      }

      &.active {
        background: rgba(218, 30, 40, 0.2);
        color: #DA1E28;
        font-weight: 600;
      }

      &:not(:last-child) {
        border-bottom: 1px solid rgba(255, 255, 255, 0.05);
      }
    }
  }
}

@media (max-width: 480px) {
  .language-switcher {
    .language-button {
      padding: 0.625rem 0.875rem;

      .current-locale {
        display: none;
      }
    }
  }
}
</style>
