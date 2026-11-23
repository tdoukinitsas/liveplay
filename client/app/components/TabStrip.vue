<template>
  <!--
    The tabs of the playlist or the cart (#74). With one tab there is nothing to
    switch between, so all that shows is a small + beside the pane's title (and,
    on the cart, the way to change how many carts it has). Editing happens in a
    small popover attached to the tab: the hover pencil, right-click or a long
    press all open it, so it works the same with a mouse and on a touch screen.
    Show Mode locks all of it, leaving only the switching.
  -->
  <div v-if="multi || !locked" class="tab-strip" :class="{ 'tab-strip--multi': multi }">
    <div v-if="multi" ref="listEl" class="tab-list" role="tablist" :aria-label="t(`tabs.${kind}Label`)">
      <div
        v-for="(tab, i) in tabs"
        :key="tab.id"
        class="tab"
        role="tab"
        tabindex="0"
        :aria-selected="tab.id === activeId"
        :class="{
          active: tab.id === activeId,
          'drop-before': dropIndex === i,
          'drop-into': dropIntoId === tab.id,
        }"
        :style="tab.color ? { '--tab-color': tab.color } : undefined"
        :title="tabTitle(tab)"
        :draggable="!locked"
        @click="selectTab(kind, tab.id)"
        @keydown.enter.prevent="selectTab(kind, tab.id)"
        @keydown.space.prevent.stop="selectTab(kind, tab.id)"
        @contextmenu.prevent="!locked && openEditor(tab.id, $event.currentTarget as HTMLElement)"
        @pointerdown="startLongPress(tab.id, $event)"
        @pointerup="cancelLongPress"
        @pointercancel="cancelLongPress"
        @pointerleave="cancelLongPress"
        @dragstart="onDragStart(tab, $event)"
        @dragend="clearDrag"
        @dragover="onDragOver(i, tab, $event)"
        @dragleave="onDragLeave(tab)"
        @drop="onDrop(i, tab, $event)"
      >
        <span v-if="indicatorFor(kind, tab.id).playing" class="tab-dot tab-dot--playing" :title="t('status.playing')" />
        <span
          v-else-if="indicatorFor(kind, tab.id).upNext !== null"
          class="material-symbols-rounded tab-next"
          :title="t('tabs.upNextOn', { name: indicatorFor(kind, tab.id).upNext ?? '' })"
        >fast_forward</span>
        <span class="tab-name">{{ tab.name || fallbackName(i) }}</span>
        <button
          v-if="!locked"
          type="button"
          class="tab-edit"
          :title="t('tabs.edit')"
          :aria-label="t('tabs.edit')"
          @click.stop="openEditor(tab.id, ($event.currentTarget as HTMLElement).parentElement as HTMLElement)"
          @pointerdown.stop
        >
          <span class="material-symbols-rounded">edit</span>
        </button>
      </div>
    </div>

    <button
      v-if="!multi && kind === 'cart' && !locked"
      ref="singleEditEl"
      type="button"
      class="tab-add"
      :title="t('tabs.cartCount')"
      :aria-label="t('tabs.cartCount')"
      @click="openEditor(tabs[0]!.id, singleEditEl!)"
    >
      <span class="material-symbols-rounded">grid_view</span>
    </button>
    <button
      v-if="!locked"
      type="button"
      class="tab-add"
      :disabled="!canAdd(kind)"
      :title="canAdd(kind) ? t(`tabs.add_${kind}`) : t('tabs.limitReached')"
      :aria-label="t(`tabs.add_${kind}`)"
      @click="onAdd"
    >
      <span class="material-symbols-rounded">add</span>
    </button>

    <div v-if="followLabel" class="tab-follow" :class="{ lost: followLost }">
      <span class="material-symbols-rounded">{{ followLost ? 'link_off' : 'visibility' }}</span>
      <span class="tab-follow-text">
        {{ followLost ? t('tabs.followLost', { name: followLabel }) : t('tabs.following', { name: followLabel }) }}
      </span>
      <button type="button" class="tab-edit tab-edit--shown" :title="t('tabs.followStop')" @click="setFollowing('')">
        <span class="material-symbols-rounded">close</span>
      </button>
    </div>

    <Teleport to="#app">
      <div v-if="editing" class="tab-popover-scrim" @pointerdown.self="closeEditor" />
      <div
        v-if="editing"
        ref="popoverEl"
        class="tab-popover"
        role="dialog"
        :aria-label="t('tabs.edit')"
        :style="popoverStyle"
        @keydown.esc.stop.prevent="closeEditor"
        @keydown.stop
      >
        <template v-if="multi">
          <label class="tab-field">
            <span>{{ t('tabs.name') }}</span>
            <input ref="nameEl" v-model="draftName" type="text" maxlength="60" @input="applyName" @keydown.enter.prevent="closeEditor" />
          </label>

          <div class="tab-field">
            <span>{{ t('tabs.colour') }}</span>
            <div class="tab-swatches">
              <button
                type="button"
                class="tab-swatch tab-swatch--none"
                :class="{ selected: !editing.color }"
                :title="t('tabs.noColour')"
                :aria-label="t('tabs.noColour')"
                @click="updateTab(kind, editing.id, { color: undefined })"
              />
              <button
                v-for="c in PRESET_COLORS"
                :key="c"
                type="button"
                class="tab-swatch"
                :class="{ selected: editing.color === c }"
                :style="{ background: c }"
                :aria-label="c"
                @click="updateTab(kind, editing.id, { color: c })"
              />
            </div>
          </div>
        </template>

        <div v-if="kind === 'cart'" class="tab-field">
          <span>{{ t('tabs.cartCount') }}</span>
          <div class="tab-stepper">
            <button type="button" :disabled="sizeOf(editing) <= minSize" :aria-label="t('tabs.fewer')" @click="stepSize(-1)">
              <span class="material-symbols-rounded">remove</span>
            </button>
            <input
              type="number"
              :min="minSize"
              :max="limits.maxCartsPerPage"
              :value="sizeOf(editing)"
              :aria-label="t('tabs.cartCount')"
              @change="onSizeInput"
            />
            <button type="button" :disabled="sizeOf(editing) >= limits.maxCartsPerPage" :aria-label="t('tabs.more')" @click="stepSize(1)">
              <span class="material-symbols-rounded">add</span>
            </button>
          </div>
          <div class="tab-presets">
            <button
              v-for="n in SIZE_PRESETS"
              :key="n"
              type="button"
              :class="{ selected: sizeOf(editing) === n }"
              :disabled="n < minSize"
              @click="setSize(n)"
            >{{ n }}</button>
          </div>
          <span v-if="minSize > limits.minCartsPerPage" class="tab-help">
            {{ t('tabs.cartCountFloor', { n: minSize }) }}
          </span>
        </div>

        <label v-if="kind === 'playlist' && multi" class="tab-field">
          <span>{{ t('tabs.atEnd') }}</span>
          <select
            :value="(editing as PlaylistTab).atEnd ?? 'wrap'"
            @change="updateTab(kind, editing!.id, { atEnd: ($event.target as HTMLSelectElement).value as PlaylistTab['atEnd'] })"
          >
            <option value="wrap">{{ t('tabs.atEndWrap') }}</option>
            <option value="continue">{{ t('tabs.atEndContinue') }}</option>
            <option value="stop">{{ t('tabs.atEndStop') }}</option>
          </select>
        </label>

        <button v-if="multi && canPopOut" type="button" class="tab-action" @click="popOut">
          <span class="material-symbols-rounded">open_in_new</span>
          {{ t('tabs.popOut') }}
        </button>

        <template v-if="multi">
          <button
            v-if="!confirmingDelete"
            type="button"
            class="tab-action tab-action--danger"
            :disabled="usage.playing"
            :title="usage.playing ? t('tabs.deletePlaying') : undefined"
            @click="confirmingDelete = true"
          >
            <span class="material-symbols-rounded">delete</span>
            {{ t('tabs.delete') }}
          </button>
          <div v-else class="tab-confirm">
            <p>
              {{ usage.count
                ? t(`tabs.deleteConfirm_${kind}`, { name: editing.name, count: usage.count })
                : t('tabs.deleteEmpty', { name: editing.name }) }}
            </p>
            <div>
              <button type="button" class="tab-action tab-action--danger" :disabled="usage.playing" @click="doDelete">
                {{ t('tabs.deleteYes') }}
              </button>
              <button type="button" class="tab-action" @click="confirmingDelete = false">{{ t('common.cancel') }}</button>
            </div>
          </div>
          <p v-if="usage.playing" class="tab-help">{{ t('tabs.deletePlaying') }}</p>
        </template>
      </div>
    </Teleport>
  </div>
</template>

<script setup lang="ts">
import type { CartPage, PlaylistTab } from '~/types/project';
import { PRESET_COLORS } from '~/types/project';
import type { TabKind } from '~/composables/useShowTabs';
import { isSecondaryWindow } from '~/composables/useProject';

const props = defineProps<{ kind: TabKind }>();

const SIZE_PRESETS = [4, 8, 12, 16, 24, 32, 48, 64];
const LONG_PRESS_MS = 500;

const { t } = useLocalization();
const { uiMode } = useUiMode();
const {
  limits, playlistTabs, cartPages, activePlaylistTabId, activeCartPageId,
  selectTab, canAdd, addTab, updateTab, deleteTab, reorderTab, tabUsage, indicatorFor,
  minCartsFor, moveItemsToTab, moveCartToPage, canPopOut, openTabWindow,
  followLabel, followLost, setFollowing,
} = useShowTabs();

const tabs = computed<(PlaylistTab | CartPage)[]>(() =>
  props.kind === 'playlist' ? playlistTabs.value : cartPages.value);
const activeId = computed(() => (props.kind === 'playlist' ? activePlaylistTabId.value : activeCartPageId.value));
const multi = computed(() => tabs.value.length > 1);
// A pop-out can switch tabs but not edit them: only the main window saves.
const locked = computed(() => uiMode.value === 'playback' || isSecondaryWindow);

const fallbackName = (i: number) =>
  t(props.kind === 'playlist' ? 'tabs.defaultPlaylist' : 'tabs.defaultPage', { n: i + 1 });

function tabTitle(tab: PlaylistTab | CartPage) {
  const upNext = indicatorFor(props.kind, tab.id).upNext;
  return upNext !== null ? t('tabs.upNextOn', { name: upNext }) : tab.name;
}

// ---- The editor ---------------------------------------------------------
const editingId = ref<string | null>(null);
const editing = computed(() => tabs.value.find(x => x.id === editingId.value) ?? null);
const usage = computed(() => (editingId.value ? tabUsage(props.kind, editingId.value) : { count: 0, playing: false }));
const minSize = computed(() => (editingId.value ? minCartsFor(editingId.value) : limits.value.minCartsPerPage));
const sizeOf = (tab: PlaylistTab | CartPage) => (tab as CartPage).size ?? limits.value.defaultCartsPerPage;

const singleEditEl = ref<HTMLElement | null>(null);
const popoverEl = ref<HTMLElement | null>(null);
const nameEl = ref<HTMLInputElement | null>(null);
const popoverStyle = ref<Record<string, string>>({});
const draftName = ref('');
const confirmingDelete = ref(false);

function openEditor(id: string, anchor: HTMLElement) {
  if (locked.value) return;
  editingId.value = id;
  confirmingDelete.value = false;
  // Held here rather than read from the tab: a save coming back from the
  // server must not rewrite the field under someone still typing.
  draftName.value = tabs.value.find(x => x.id === id)?.name ?? '';
  const r = anchor.getBoundingClientRect();
  popoverStyle.value = { left: `${r.left}px`, top: `${r.bottom + 4}px`, visibility: 'hidden' };
  void nextTick(() => {
    const el = popoverEl.value;
    if (!el) return;
    const margin = 8;
    const box = el.getBoundingClientRect();
    const left = Math.max(margin, Math.min(r.left, window.innerWidth - box.width - margin));
    const fitsBelow = r.bottom + 4 + box.height <= window.innerHeight - margin;
    const top = fitsBelow ? r.bottom + 4 : Math.max(margin, r.top - box.height - 4);
    popoverStyle.value = { left: `${left}px`, top: `${top}px` };
    nameEl.value?.select();
  });
}

function closeEditor() {
  editingId.value = null;
  confirmingDelete.value = false;
}

// A tab removed by another operator closes its own editor.
watch(editing, (tab) => { if (!tab) closeEditor(); });
watch(locked, (isLocked) => { if (isLocked) closeEditor(); });

function applyName() {
  if (editingId.value) updateTab(props.kind, editingId.value, { name: draftName.value });
}

function setSize(n: number) {
  if (editingId.value) updateTab(props.kind, editingId.value, { size: n });
}
function stepSize(delta: number) {
  if (editing.value) setSize(sizeOf(editing.value) + delta);
}
function onSizeInput(e: Event) {
  const input = e.target as HTMLInputElement;
  const n = parseInt(input.value, 10);
  if (Number.isFinite(n)) setSize(Math.max(limits.value.minCartsPerPage, n));
  if (editing.value) input.value = String(sizeOf(editing.value));
}

function doDelete() {
  if (editingId.value && deleteTab(props.kind, editingId.value)) closeEditor();
}

function popOut() {
  if (!editingId.value) return;
  openTabWindow(props.kind, editingId.value);
  closeEditor();
}

// Straight to naming a new tab. Held in shared state because adding the second
// tab replaces this strip (the + beside the title) with the full tab bar, and
// it's that new instance which has a tab to attach the editor to.
const pendingEdit = useState<{ kind: TabKind; id: string } | null>('showTabs.pendingEdit', () => null);

function onAdd() {
  const id = addTab(props.kind);
  if (id) pendingEdit.value = { kind: props.kind, id };
}

function openPendingEdit() {
  const pending = pendingEdit.value;
  if (!pending || pending.kind !== props.kind || !multi.value) return;
  void nextTick(() => {
    const el = listEl.value?.querySelector<HTMLElement>('.tab.active');
    if (!el) return;
    pendingEdit.value = null;
    el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    openEditor(pending.id, el);
  });
}
onMounted(openPendingEdit);
watch(pendingEdit, openPendingEdit);

// ---- Long press (touch) --------------------------------------------------
let longPress: ReturnType<typeof setTimeout> | null = null;
function startLongPress(id: string, e: PointerEvent) {
  if (locked.value || e.pointerType === 'mouse') return;
  const target = e.currentTarget as HTMLElement;
  cancelLongPress();
  longPress = setTimeout(() => openEditor(id, target), LONG_PRESS_MS);
}
function cancelLongPress() {
  if (longPress) clearTimeout(longPress);
  longPress = null;
}
onUnmounted(cancelLongPress);

// ---- Drag: tabs to reorder, cues and carts onto a tab to move them --------
const listEl = ref<HTMLElement | null>(null);
const dropIndex = ref<number | null>(null);
const dropIntoId = ref<string | null>(null);
const TAB_TYPE = computed(() => `liveplay-tab-${props.kind}`);

function clearDrag() {
  dropIndex.value = null;
  dropIntoId.value = null;
}

function onDragStart(tab: PlaylistTab | CartPage, e: DragEvent) {
  if (locked.value || !e.dataTransfer) return;
  e.dataTransfer.effectAllowed = 'move';
  e.dataTransfer.setData(TAB_TYPE.value, tab.id);
  // A playlist tab can also be dropped on a cart, which then starts it (#61).
  if (props.kind === 'playlist') e.dataTransfer.setData('liveplay-playlist-tab', tab.id);
}

const dragTypes = (e: DragEvent) => Array.from(e.dataTransfer?.types ?? []);
const carriesContent = (e: DragEvent) => {
  const types = dragTypes(e);
  return props.kind === 'cart'
    ? types.includes('cart-slot')
    : types.includes('item-uuid') && !types.includes('cart-slot');
};

function onDragOver(i: number, tab: PlaylistTab | CartPage, e: DragEvent) {
  if (locked.value) return;
  if (dragTypes(e).includes(TAB_TYPE.value)) {
    e.preventDefault();
    dropIndex.value = i;
    dropIntoId.value = null;
  } else if (carriesContent(e)) {
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'move';
    dropIntoId.value = tab.id;
    dropIndex.value = null;
  }
}

function onDragLeave(tab: PlaylistTab | CartPage) {
  if (dropIntoId.value === tab.id) dropIntoId.value = null;
}

function onDrop(i: number, tab: PlaylistTab | CartPage, e: DragEvent) {
  if (locked.value || !e.dataTransfer) return;
  const movedTab = e.dataTransfer.getData(TAB_TYPE.value);
  if (movedTab) {
    e.preventDefault();
    e.stopPropagation();
    const from = tabs.value.findIndex(x => x.id === movedTab);
    reorderTab(props.kind, movedTab, from < i ? i - 1 : i);
  } else if (carriesContent(e)) {
    e.preventDefault();
    e.stopPropagation();
    if (props.kind === 'cart') {
      moveCartToPage(parseInt(e.dataTransfer.getData('cart-slot'), 10), tab.id);
    } else {
      let uuids: string[] = [];
      try { uuids = JSON.parse(e.dataTransfer.getData('selected-items') || '[]'); } catch { /* single item */ }
      const dragged = e.dataTransfer.getData('item-uuid');
      moveItemsToTab(uuids.includes(dragged) ? uuids : [dragged], tab.id);
    }
  }
  clearDrag();
}
</script>

<style scoped lang="scss">
.tab-strip {
  display: flex;
  align-items: center;
  gap: 4px;
  min-width: 0;
}

.tab-strip--multi {
  flex: 0 0 auto;
  padding: 4px var(--spacing-md) 0;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
}

.tab-list {
  display: flex;
  align-items: flex-end;
  gap: 2px;
  min-width: 0;
  overflow-x: auto;
  scrollbar-width: thin;
}

.tab {
  --tab-color: transparent;
  position: relative;
  display: flex;
  align-items: center;
  gap: 6px;
  flex: 0 0 auto;
  max-width: 200px;
  min-height: 34px;
  padding: 0 10px;
  border: 1px solid transparent;
  border-bottom: none;
  border-top: 3px solid var(--tab-color);
  border-radius: 6px 6px 0 0;
  color: var(--color-text-secondary);
  font-size: 13px;
  cursor: pointer;
  user-select: none;

  &:hover { background-color: var(--color-surface-hover); color: var(--color-text-primary); }
  &:focus-visible { outline: 2px solid var(--color-accent); outline-offset: -2px; }

  &.active {
    background-color: var(--color-background);
    border-left-color: var(--color-border);
    border-right-color: var(--color-border);
    color: var(--color-text-primary);
    font-weight: 600;
  }

  &.drop-before::before {
    content: '';
    position: absolute;
    left: -2px;
    top: 2px;
    bottom: 2px;
    width: 3px;
    border-radius: 2px;
    background: var(--color-accent);
  }

  &.drop-into { outline: 2px solid var(--color-accent); outline-offset: -2px; }
}

.tab-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.tab-dot {
  flex: 0 0 auto;
  width: 8px;
  height: 8px;
  border-radius: 50%;
}
.tab-dot--playing { background: var(--color-success); }
.tab-next {
  flex: 0 0 auto;
  font-size: 16px;
  color: var(--color-warning);
}

.tab-edit {
  display: none;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  width: 22px;
  height: 22px;
  margin-right: -4px;
  padding: 0;
  border: none;
  border-radius: 4px;
  background: none;
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded { font-size: 15px; }
  &:hover { background: var(--color-surface-hover); color: var(--color-text-primary); }
}
.tab-edit--shown { display: flex; }
.tab:hover .tab-edit,
.tab:focus-within .tab-edit { display: flex; }

.tab-add {
  display: flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  width: 28px;
  height: 28px;
  padding: 0;
  border: none;
  border-radius: 6px;
  background: none;
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded { font-size: 20px; }
  &:hover:not(:disabled) { background: var(--color-surface-hover); color: var(--color-text-primary); }
  &:disabled { opacity: 0.4; cursor: not-allowed; }
}

.tab-follow {
  display: flex;
  align-items: center;
  gap: 4px;
  min-width: 0;
  margin-left: auto;
  padding: 0 4px 0 8px;
  font-size: 12px;
  color: var(--color-text-secondary);

  .material-symbols-rounded { font-size: 16px; }
  &.lost { color: var(--color-warning); }
}
.tab-follow-text {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

</style>

<!-- Not scoped: the popover is teleported, and scoped ids don't reliably reach a
     teleported root in packaged builds. Every class here starts with tab-. -->
<style lang="scss">
.tab-popover-scrim {
  position: fixed;
  inset: 0;
  z-index: 8999;
}

.tab-popover {
  position: fixed;
  z-index: 9000;
  display: flex;
  flex-direction: column;
  gap: 12px;
  width: 270px;
  max-width: calc(100vw - 16px);
  max-height: calc(100vh - 16px);
  overflow-y: auto;
  padding: 12px;
  border: 1px solid var(--color-border);
  border-radius: 8px;
  background: var(--color-surface);
  color: var(--color-text-primary);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
  font-size: 13px;
}

.tab-field {
  display: flex;
  flex-direction: column;
  gap: 5px;

  > span:first-child {
    font-size: 12px;
    color: var(--color-text-secondary);
  }

  input, select {
    width: 100%;
    box-sizing: border-box;
    padding: 6px 8px;
    border: 1px solid var(--color-border);
    border-radius: 4px;
    background: var(--color-background);
    color: var(--color-text-primary);
    font: inherit;
  }
}

.tab-help {
  margin: 0;
  font-size: 12px;
  color: var(--color-text-secondary);
}

.tab-swatches {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}
.tab-swatch {
  width: 22px;
  height: 22px;
  padding: 0;
  border: 2px solid transparent;
  border-radius: 50%;
  cursor: pointer;

  &.selected { border-color: var(--color-text-primary); }
}
.tab-swatch--none {
  background:
    linear-gradient(135deg, transparent 45%, var(--color-danger) 45%, var(--color-danger) 55%, transparent 55%),
    var(--color-background);
  box-shadow: inset 0 0 0 1px var(--color-border);
}

.tab-stepper {
  display: flex;
  gap: 4px;

  input { flex: 1; min-width: 0; text-align: center; }
  button {
    display: flex;
    align-items: center;
    justify-content: center;
    flex: 0 0 32px;
    border: 1px solid var(--color-border);
    border-radius: 4px;
    background: var(--color-background);
    color: var(--color-text-primary);
    cursor: pointer;

    .material-symbols-rounded { font-size: 18px; }
    &:disabled { opacity: 0.4; cursor: not-allowed; }
  }
}

.tab-presets {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 4px;

  button {
    padding: 4px 0;
    border: 1px solid var(--color-border);
    border-radius: 4px;
    background: var(--color-background);
    color: var(--color-text-primary);
    font: inherit;
    cursor: pointer;

    &:hover:not(:disabled) { background: var(--color-surface-hover); }
    &.selected { border-color: var(--color-accent); color: var(--color-accent); font-weight: 600; }
    &:disabled { opacity: 0.4; cursor: not-allowed; }
  }
}

.tab-action {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 6px 10px;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  background: var(--color-background);
  color: var(--color-text-primary);
  font: inherit;
  cursor: pointer;

  .material-symbols-rounded { font-size: 17px; }
  &:hover:not(:disabled) { background: var(--color-surface-hover); }
  &:disabled { opacity: 0.45; cursor: not-allowed; }
}
.tab-action--danger { color: var(--color-danger); }

.tab-confirm {
  display: flex;
  flex-direction: column;
  gap: 8px;

  p { margin: 0; }
  > div { display: flex; gap: 6px; }
  .tab-action { flex: 1; }
}
</style>
