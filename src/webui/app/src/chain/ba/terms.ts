// The words of BitAssets in the glossary of the kit (components/Term.svelte), beside the kit's own (pool,
// slippage, fee...).
import { addTerms } from '../../lib/glossary';

addTerms({
  asset: { term: 'token', text: 'An asset of Flush: points, shares, tickets, a stablecoin. Anyone can create one; it is sent like a coin.' },
  controlcoin: { term: 'control coin', text: 'The coin that lets the creator of a token mint more of it and change its description. Whoever holds it controls the token.' },
  pricequote: { term: 'quote', text: 'What the market would give you now for what you pay. The real amount can be a little less if someone trades first.' },
});
