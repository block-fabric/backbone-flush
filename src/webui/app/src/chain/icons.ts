// Icons of this chain's own pages, beside those of the kit (components/Icon.svelte): line icons on a
// 24 grid with a 1.75 stroke, one SVG path each. The pages of Layers name them in chain/local.ts: FastBlocks'
// blocks of stake first, then BitAssets' assets.
export const LOCAL_ICONS = {
  /** Staking: coins stacked under a key-shaped lock. */
  stake: 'M4 17.5c0 1.4 2.7 2.5 6 2.5s6-1.1 6-2.5M4 17.5v-4c0-1.4 2.7-2.5 6-2.5s6 1.1 6 2.5v4M4 13.5c0 1.4 2.7 2.5 6 2.5s6-1.1 6-2.5M15 8.5V6a3 3 0 0 1 6 0v2.5M14 8.5h8v5h-8z',
  /** Finality & BMM: a stopwatch with a check. */
  finality: 'M12 21a8 8 0 1 0 0-16 8 8 0 0 0 0 16zM10 2.5h4M12 2.5V5M18.4 6.6l1.4-1.4M8.6 13.2l2.4 2.4 4.4-4.6',
  /** Slashing & jail: a gavel (FastBlocks' gavel; BitAssets' auctions keep the name gavel). */
  slash: 'M13.5 4.5l6 6M11.5 6.5l6 6M14.8 5.8l-5.3 5.3M18.2 9.2l-5.3 5.3M11.2 11.8L4 19M3 21h9',
  /** Assets: a cut gem. */
  gem: 'M7 4h10l4 5.5L12 21 3 9.5zM3 9.5h18M9.5 4L8 9.5l4 11.5 4-11.5L14.5 4',
  /** My assets: coins stacked, one standing. */
  holdings: 'M4 16.5c0 1.4 2.7 2.5 6 2.5s6-1.1 6-2.5M4 12.5c0 1.4 2.7 2.5 6 2.5s6-1.1 6-2.5M4 8.5C4 7.1 6.7 6 10 6s6 1.1 6 2.5S13.3 11 10 11 4 9.9 4 8.5zM4 8.5v8M16 8.5v8M18 7.2c1.2.4 2 1 2 1.8v6.5c0 .8-.8 1.4-2 1.8',
  /** Issue an asset: a gem with a spark. */
  issue: 'M5 8h8l3 4-7 8-7-8zM2 12h14M18.5 2.5v5M16 5h5',
  /** Trade: two arrows the opposite way. */
  swap: 'M4 8h14M14 4l4 4-4 4M20 16H6M10 12l-4 4 4 4',
  /** A pool: a drop over a level. */
  pool: 'M12 3.5s-5.5 6-5.5 9.8a5.5 5.5 0 0 0 11 0C17.5 9.5 12 3.5 12 3.5zM3 20.5h18',
  /** Auctions: a gavel on its block. */
  gavel: 'M13.5 4.5l6 6M11 7l6 6M9.8 8.2l6 6M12.2 5.8l-5 5 2 2 5-5M8.5 11.5L3.5 16.5M4 20.5h10',
  /** Activity: a list with a pulse. */
  pulse: 'M3 12h4l2.5-6 4 12 2.5-6H21',
  /** Burn: a flame. */
  flame: 'M12 21a6 6 0 0 0 6-6c0-3.5-2.5-5.5-3.5-8.5-1 2-2 3-3.5 3.5C11 7.5 10 5 9 3 7.5 6 6 8.5 6 12.5V15a6 6 0 0 0 6 6zM12 21a2.5 2.5 0 0 1-2.5-2.5c0-1.5 1.2-2.4 2.5-3.5 1.3 1.1 2.5 2 2.5 3.5A2.5 2.5 0 0 1 12 21z',
  /** Back to a list. */
  back: 'M15 5l-7 7 7 7',
  /** A logo: a picture, a hill and a sun in a frame. */
  picture: 'M4 5h16v14H4zM4 16l5-5 4 4 2-2 5 5M15.5 9.5a1.5 1.5 0 1 0 0-.01',
  /** The control coin: a crown. */
  crown: 'M4 17l-1-9 5 4 4-7 4 7 5-4-1 9zM5 20h14',
} as const satisfies Record<string, string>;
