<script lang="ts">
  // While the node starts (or stops): what it does now.
  import { app } from '../lib/state.svelte';
  import { CHAIN } from '../chain/config';
  import { host } from '../lib/rpc';

  let { stopping = false }: { stopping?: boolean } = $props();

  function onkeydown(e: KeyboardEvent) {
    // As the splash screen of chains-qt: q stops the start, to continue later.
    if (!stopping && (e.key === 'q' || e.key === 'Q')) host.quit();
  }
</script>

<svelte:window {onkeydown} />

<main class="splash" aria-busy="true">
  <!-- The Launchpad looks (styles/launchpad.css): a still horizon and a few stars, painted once; the chain's own mark. -->
  <div class="sky" aria-hidden="true"><span class="stars"></span></div>
  <svg class="brandmark" viewBox="0 0 32 32" aria-hidden="true">
    {#each CHAIN.brand.mark as d, i (i)}<path {d} />{/each}
  </svg>
  <svg class="mark" viewBox="0 0 64 32" aria-hidden="true">
    <rect class="a" x="4" y="6" width="20" height="20" rx="6" />
    <rect class="b" x="40" y="6" width="20" height="20" rx="6" />
    <path d="M24 16h16" />
  </svg>
  <h1>{app.info?.name ?? CHAIN.name}</h1>
  {#if app.info?.networkLabel}<span class="badge warning">{app.info.networkLabel}</span>{/if}
  {#if stopping}
    <p class="message" role="status">{app.info?.name ?? CHAIN.name} is shutting down…</p>
    <p class="hint">Do not shut down the computer until this window disappears.</p>
  {:else}
    <p class="message" role="status">{app.initMessage || 'Starting…'}</p>
    {#if app.progress}
      <progress max="100" value={app.progress.progress} aria-label={app.progress.title}></progress>
    {/if}
    <p class="hint">Press <kbd>q</kbd> to shut down and continue later.</p>
  {/if}
  {#if app.info}<p class="version faint">{app.info.version}</p>{/if}
</main>

<style>
  .splash {
    position: relative;
    isolation: isolate;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 0.6rem;
    height: 100%;
    padding: 2rem;
    text-align: center;
  }
  .mark {
    width: 6rem;
    fill: none;
    stroke: var(--accent);
    stroke-width: 3;
  }
  .mark .b {
    animation: link 1.6s ease-in-out infinite alternate;
  }
  @keyframes link {
    from {
      opacity: 0.25;
    }
  }
  h1 {
    font-size: 2rem;
  }
  .message {
    min-height: 1.5em;
    color: var(--text-muted);
    max-width: 40rem;
  }
  .hint {
    color: var(--text-faint);
    font-size: 0.9rem;
  }
  progress {
    width: 16rem;
    accent-color: var(--accent);
  }
  .version {
    font-size: 0.8rem;
  }
  .sky,
  .brandmark {
    display: none;
  }
  .brandmark {
    width: 6rem;
    fill: none;
    stroke: var(--text);
    stroke-width: 1.6;
    stroke-linecap: square;
  }
  .sky {
    position: absolute;
    inset: 0;
    z-index: -1;
    overflow: hidden;
    pointer-events: none;
    /* The curve of a horizon low on the screen, lit faintly from below. */
    background: radial-gradient(85vw 34vh at 50% 108%, rgb(175 200 225 / 42%), rgb(95 120 145 / 14%) 48%, transparent 72%);
  }
  .stars {
    position: absolute;
    left: 0;
    top: 0;
    width: 1px;
    height: 1px;
    border-radius: 50%;
    box-shadow: 11vw 27vh 0 0 rgb(255 255 255 / 35%), 83vw 35vh 0 0 rgb(255 255 255 / 25%), 5vw 6vh 0 0 rgb(255 255 255 / 25%), 5vw 10vh 0 0.5px rgb(255 255 255 / 45%), 69vw 23vh 0 0.5px rgb(255 255 255 / 25%), 52vw 47vh 0 0 rgb(255 255 255 / 45%), 21vw 32vh 0 0 rgb(255 255 255 / 25%), 60vw 49vh 0 0.5px rgb(255 255 255 / 25%), 57vw 16vh 0 0 rgb(255 255 255 / 60%), 93vw 19vh 0 0 rgb(255 255 255 / 25%), 78vw 24vh 0 0 rgb(255 255 255 / 45%), 60vw 57vh 0 0 rgb(255 255 255 / 35%), 51vw 21vh 0 0 rgb(255 255 255 / 35%), 16vw 39vh 0 0 rgb(255 255 255 / 45%), 10vw 50vh 0 0 rgb(255 255 255 / 25%), 49vw 37vh 0 0 rgb(255 255 255 / 45%), 69vw 50vh 0 0.5px rgb(255 255 255 / 35%), 94vw 36vh 0 0 rgb(255 255 255 / 25%), 52vw 8vh 0 0.5px rgb(255 255 255 / 45%), 24vw 38vh 0 0 rgb(255 255 255 / 35%), 40vw 27vh 0 0.5px rgb(255 255 255 / 25%), 68vw 43vh 0 0.5px rgb(255 255 255 / 25%), 27vw 51vh 0 0 rgb(255 255 255 / 45%), 12vw 6vh 0 0 rgb(255 255 255 / 60%), 85vw 13vh 0 0 rgb(255 255 255 / 60%), 71vw 51vh 0 0 rgb(255 255 255 / 60%), 50vw 47vh 0 0 rgb(255 255 255 / 60%), 86vw 21vh 0 0 rgb(255 255 255 / 35%), 50vw 4vh 0 0 rgb(255 255 255 / 25%), 27vw 9vh 0 0 rgb(255 255 255 / 45%), 37vw 43vh 0 0.5px rgb(255 255 255 / 45%), 9vw 27vh 0 0 rgb(255 255 255 / 45%), 51vw 33vh 0 0 rgb(255 255 255 / 45%), 73vw 4vh 0 0 rgb(255 255 255 / 25%), 80vw 23vh 0 0.5px rgb(255 255 255 / 60%), 36vw 49vh 0 0 rgb(255 255 255 / 60%), 9vw 47vh 0 0 rgb(255 255 255 / 45%), 83vw 29vh 0 0.5px rgb(255 255 255 / 25%), 11vw 11vh 0 0.5px rgb(255 255 255 / 35%), 4vw 25vh 0 0 rgb(255 255 255 / 35%), 88vw 2vh 0 0 rgb(255 255 255 / 60%), 13vw 40vh 0 0 rgb(255 255 255 / 25%);
  }
  :global(:root[data-look='launchpad']) .sky,
  :global(:root[data-look='launchpad']) .brandmark {
    display: block;
  }
  :global(:root[data-look='launchpad']) .mark,
  :global(:root[data-look='launchpad']) .mark .b {
    display: none;
    animation: none;
  }
  :global(:root[data-theme='launchpadday']) .stars {
    display: none;
  }
  :global(:root[data-theme='launchpadday']) .sky {
    background: radial-gradient(85vw 34vh at 50% 108%, rgb(44 61 79 / 16%), rgb(44 61 79 / 5%) 48%, transparent 72%);
  }
  :global(:root[data-look='launchpad']) h1 {
    margin-top: 0.6rem;
    font-size: 2.6rem;
    font-weight: 300;
    letter-spacing: 0.42em;
    /* The tracking after the last letter, given back so the word stays centred. */
    margin-right: -0.42em;
  }
  :global(:root[data-look='launchpad']) .message {
    font-family: var(--font-mono);
    font-size: 0.8rem;
    text-transform: uppercase;
    letter-spacing: 0.12em;
  }
  :global(:root[data-look='launchpad']) progress {
    width: 20rem;
  }
</style>
