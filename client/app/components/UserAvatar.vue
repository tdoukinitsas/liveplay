<template>
  <!-- A person's picture, or their initials on a colour of their own when they
       have none. The colour comes from the name rather than from a palette
       index, so the same person is the same colour on every surface and after
       every reload without anything having to be stored. -->
  <span class="user-avatar" role="img" :aria-label="label || name"
        :style="{
          width: `${size}px`, height: `${size}px`,
          fontSize: `${Math.round(size * 0.4)}px`,
          background: src ? 'transparent' : colour,
        }">
    <img v-if="src" :src="src" alt="" draggable="false" />
    <span v-else aria-hidden="true">{{ initials }}</span>
  </span>
</template>

<script setup lang="ts">
const props = withDefaults(defineProps<{
  name?: string | null;
  src?: string | null;
  size?: number;
  label?: string;
}>(), { name: '', src: null, size: 32, label: '' });

// Up to two letters: the first of the first two words, or the first two of a
// single word. Array.from rather than indexing so a name that starts with an
// emoji or an accented letter made of two code units is not cut in half.
const initials = computed(() => {
  const words = String(props.name ?? '').trim().split(/[\s._-]+/).filter(Boolean);
  if (!words.length) return '?';
  const first = Array.from(words[0]!);
  const letters = words.length > 1
    ? [first[0], Array.from(words[1]!)[0]]
    : first.slice(0, 2);
  return letters.join('').toUpperCase();
});

// A stable hue per name. Saturation and lightness are fixed so every colour
// carries white text legibly in both themes.
const colour = computed(() => {
  let h = 0;
  for (const ch of String(props.name ?? '')) h = (h * 31 + ch.codePointAt(0)!) >>> 0;
  return `hsl(${h % 360}, 45%, 42%)`;
});
</script>

<style lang="scss" scoped>
.user-avatar {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
  border-radius: 50%;
  overflow: hidden;
  color: #fff;
  font-weight: 600;
  line-height: 1;
  user-select: none;
  img { width: 100%; height: 100%; object-fit: cover; display: block; }
}
</style>
