<template>
  <div class="project-header">
    <div class="header-left">
      <img 
        :src="isDark ? './assets/icons/SVG/liveplay-icon-darkmode@web.svg' : './assets/icons/SVG/liveplay-icon-lightmode@web.svg'"
        alt="LivePlay"
        class="header-logo"
      />
      <h2 class="project-name">{{ currentProject?.name || t('project.noProject') }}</h2>
    </div>
    
    <div class="header-right">
      <div class="digital-clock">{{ currentTime }}</div>
    </div>
  </div>
</template>

<script setup lang="ts">
const { currentProject } = useProject();
const { t } = useLocalization();

const isDark = computed(() => currentProject.value?.theme.mode === 'dark');
const currentTime = ref('00:00:00');

const updateClock = () => {
  const now = new Date();
  const hours = now.getHours().toString().padStart(2, '0');
  const minutes = now.getMinutes().toString().padStart(2, '0');
  const seconds = now.getSeconds().toString().padStart(2, '0');
  currentTime.value = `${hours}:${minutes}:${seconds}`;
};

onMounted(() => {
  updateClock();
  const interval = setInterval(updateClock, 1000);
  
  onUnmounted(() => {
    clearInterval(interval);
  });
});
</script>

<style scoped lang="scss">
.project-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacing-sm) var(--spacing-lg);
  background-color: var(--color-surface);
  border-bottom: 1px solid var(--color-border);
  min-height: 60px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: var(--spacing-md);
}

.header-logo {
  width: 36px;
  height: 36px;
  object-fit: contain;
}

.project-name {
  font-size: 18px;
  font-weight: 600;
  color: var(--color-text-primary);
  margin: 0;
}

.header-right {
  display: flex;
  align-items: center;
}

.digital-clock {
  font-size: 24px;
  font-weight: 700;
  color: var(--color-accent);
  letter-spacing: 0.05em;
  padding: var(--spacing-xs) var(--spacing-md);
  border: 2px solid var(--color-accent);
  border-radius: var(--border-radius-md);
  background-color: var(--color-surface);
  box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);
  transition: all var(--transition-base);
}
</style>
