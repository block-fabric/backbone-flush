<script lang="ts">
  // What the Send page sends beside the coin of the chain (an asset, a name...): a choice at the top of Send,
  // and the chain's own form, which takes the place of the kit's while `active`. The template sends only its
  // coin: nothing here.
  let { active = $bindable(false) }: { active?: boolean } = $props();
  void active;
</script>
