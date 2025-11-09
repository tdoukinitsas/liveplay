<template>
  <div class="feature-highlight">
    <div class="feature-image">
      <img 
        :src="imageSrc" 
        :alt="title"
        @error="handleImageError"
      />
    </div>
    <div class="feature-content">
      <h3 class="feature-title">{{ title }}</h3>
      <div class="feature-description">
        <slot></slot>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
interface Props {
  title: string;
  imageSrc: string;
}

defineProps<Props>();

const handleImageError = (event: Event) => {
  console.error('Failed to load image:', (event.target as HTMLImageElement).src);
};
</script>

<style scoped lang="scss">
.feature-highlight {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 3rem;
  align-items: center;
  padding: 3rem 0;
  border-bottom: 1px solid rgba(255, 255, 255, 0.1);

  &:last-child {
    border-bottom: none;
  }

  .feature-image {
    border-radius: 12px;
    overflow: hidden;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.5);
    border: 1px solid rgba(255, 255, 255, 0.1);
    transition: transform 0.3s ease, box-shadow 0.3s ease;

    &:hover {
      transform: translateY(-4px);
      box-shadow: 0 12px 50px rgba(218, 30, 40, 0.2);
    }

    img {
      width: 100%;
      height: auto;
      display: block;
    }
  }

  .feature-content {
    .feature-title {
      font-size: 2rem;
      color: #DA1E28;
      margin: 0 0 1rem;
      font-weight: 600;
    }

    .feature-description {
      font-size: 1.125rem;
      line-height: 1.8;
      color: rgba(255, 255, 255, 0.85);

      :deep(p) {
        margin: 0.75rem 0;
      }

      :deep(strong) {
        color: rgba(255, 255, 255, 0.95);
        font-weight: 600;
      }

      :deep(ul) {
        margin: 1rem 0;
        padding-left: 1.5rem;

        li {
          margin: 0.5rem 0;
        }
      }
    }
  }

  // Alternate layout for odd items
  &:nth-child(even) {
    .feature-image {
      order: 2;
    }

    .feature-content {
      order: 1;
    }
  }
}

@media (max-width: 968px) {
  .feature-highlight {
    grid-template-columns: 1fr;
    gap: 2rem;
    padding: 2rem 0;

    .feature-image {
      order: 1 !important;
    }

    .feature-content {
      order: 2 !important;

      .feature-title {
        font-size: 1.75rem;
      }

      .feature-description {
        font-size: 1rem;
      }
    }
  }
}

@media (max-width: 480px) {
  .feature-highlight {
    .feature-content {
      .feature-title {
        font-size: 1.5rem;
      }
    }
  }
}
</style>
