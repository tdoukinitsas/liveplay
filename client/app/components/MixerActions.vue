<template>
  <!--
    The mixer's own controls: add a bus, the output map, clear PFL, and the
    window buttons. Rendered as the action group of the mixer's header bar.

    They were a footer of icon-only buttons once, on the argument that a mixer
    is judged on how much of the window is fader. That made the mixer the one
    view whose top edge looked nothing like the playlist's or the cart
    player's, each of which has a title on the left and labelled buttons on
    the right. Consistency won (maintainer decision, 2026-08-22): the mixer
    now has the same header, and these are the same Btn components those
    views use.
  -->
  <div class="acts">
    <!-- Accent-styled: adding a bus is the one thing here that changes the
         show, so it reads as an action rather than a window control. -->
    <Btn
      class="acts__add"
      icon="add"
      :text="t('mixer.addBus')"
      :title="t('mixer.addBus')"
      @click="$emit('add')"
    />

    <!-- Always available, regardless of whether anything is currently
         unmapped — this is the general output-map editor as well as the D3
         "Remap Hardware Outputs" surface. -->
    <Btn
      icon="settings_input_hdmi"
      :text="t('mixer.outputsButton')"
      :title="t('mixer.outputMapButton')"
      @click="$emit('output-map')"
    />

    <!-- Only present while something is actually in the phones. PFL is
         additive and quiet about it — several channels tapped from several
         windows sound like one muddled headphone mix — so the count is on the
         button, and it is the one place that clears all of them at once. -->
    <Btn
      v-if="pflCount > 0"
      class="acts__pfl"
      icon="headphones"
      :text="t('mixer.clearPflCount', { count: pflCount })"
      :title="t('mixer.clearPfl')"
      @click="$emit('clear-pfl')"
    />

    <!-- Detached: the window IS the mixer, so the side/full toggle has nothing
         to toggle between and the only exit is back to the main window. -->
    <template v-if="!detached">
      <Btn
        v-if="canDetach"
        icon="open_in_new"
        :text="t('mixer.detach')"
        :title="t('mixer.detach')"
        @click="$emit('detach')"
      />
      <Btn
        :icon="mode === 'side' ? 'open_in_full' : 'close_fullscreen'"
        :text="mode === 'side' ? t('mixer.expand') : t('mixer.dock')"
        :title="mode === 'side' ? t('mixer.expand') : t('mixer.dock')"
        @click="$emit('mode', mode === 'side' ? 'full' : 'side')"
      />
    </template>

    <Btn
      :icon="detached ? 'dock_to_left' : 'close'"
      :text="detached ? t('mixer.dockToMain') : t('mixer.close')"
      :title="detached ? t('mixer.dockToMain') : t('mixer.close')"
      @click="$emit('close')"
    />
  </div>
</template>

<script setup lang="ts">
import Btn from './Btn.vue';

withDefaults(
  defineProps<{
    mode: 'side' | 'full';
    detached?: boolean;
    canDetach?: boolean;
    /** How many buses are currently PFL'd. Zero hides the clear control. */
    pflCount?: number;
  }>(),
  { pflCount: 0 },
);

defineEmits<{
  (e: 'add'): void;
  (e: 'detach'): void;
  (e: 'mode', mode: 'side' | 'full'): void;
  (e: 'close'): void;
  (e: 'clear-pfl'): void;
  (e: 'output-map'): void;
}>();

const { t } = useLocalization();
</script>

<style scoped>
/* Same gap as .playlist-actions / .cart-header-actions. */
.acts { display: flex; align-items: center; gap: var(--spacing-sm); flex: 0 0 auto; }

/* Btn has no accent variant and is deliberately left alone; the two mixer
   flavours are styled from outside. The class lands on Btn's root <button>
   (single-root fallthrough), which also carries this component's scope id,
   so a plain scoped selector reaches it — no :deep needed. */
.acts__add.btn {
  color: var(--color-accent);
  border-color: var(--color-accent);
}
/* Same green as a lit PFL button on a strip, so the two read as the same
   thing: this is what is in your headphones, and this is how it stops. */
.acts__pfl.btn {
  color: #fff;
  background: var(--color-success);
  border-color: var(--color-success);
}
.acts__pfl.btn:hover:not(:disabled) {
  color: #fff;
  background: var(--color-success);
  border-color: var(--color-success);
  filter: brightness(1.1);
}

/* The docked side pane can be ~220px wide. Below ~640px of mixer width the
   text labels go and the icons stay; every button keeps its title tooltip.
   The query is against the .mixer container (container-type: inline-size in
   MixerPanel.vue), not the viewport, so a narrow pane in a wide window still
   collapses. Btn renders icon span + text span, so hide the second. */
@container mixer (max-width: 640px) {
  .acts :deep(.btn > span:not(.material-symbols-rounded)) { display: none; }
}
</style>
