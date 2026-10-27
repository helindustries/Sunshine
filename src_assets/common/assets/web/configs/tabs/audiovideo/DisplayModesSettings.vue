<script setup>
import { ref } from 'vue'
const props = defineProps({
  platform: String,
  config: Object,
})
const config = ref(props.config)
</script>

<template>
  <!--virtual_display-->
  <div class="mb-3">
    <label for="virtual_display" class="form-label">{{ $t("config.virtual_display") }}</label>
    <select id="virtual_display" class="form-select" v-model="config.virtual_display">
      <option value="enabled">{{ $t("_common.enabled") }}</option>
      <option value="disabled">{{ $t("_common.disabled") }}</option>
    </select>
    <div class="form-text">{{ $t("config.virtual_display_desc") }}</div>
  </div>

  <!--max_bitrate-->
  <div class="mb-3">
    <label for="max_bitrate" class="form-label">{{ $t("config.max_bitrate") }}</label>
    <input type="number" class="form-control" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
    <div class="form-text">{{ $t("config.max_bitrate_desc") }}</div>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-3">
    <label for="minimum_fps_target" class="form-label">{{ $t("config.minimum_fps_target") }}</label>
    <input type="number" min="0" max="1000" class="form-control" id="minimum_fps_target" placeholder="0" v-model="config.minimum_fps_target" />
    <div class="form-text">{{ $t("config.minimum_fps_target_desc") }}</div>
  </div>
</template>

<style scoped>
.ms-item {
  background-color: var(--bs-dark-bg-subtle);
  font-size: 12px;
  font-weight: bold;
}
</style>
