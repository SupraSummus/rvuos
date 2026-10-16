/-
What the words of an Untyped mean, proved for every one of the 2^32 words.
The kernel keeps a block, its base and its size, in one word, `block_word`,
and reads it back with `block_base` and `block_size`, for an Untyped and a frame alike;
`OP_UNTYPED_SPLIT` makes two halves of a block.
`bv_decide` proves each statement over the 32-bit words with a SAT solver, and Lean checks its proof.
-/
import Std.Tactic.BVDecide
import Rvuos.State

namespace Rvuos

/-- A block's word reads back as the block: its size, and its base. -/
theorem blockWord_reads_back (base size : BitVec 32)
    (pow : size &&& (size - 1) = 0) (big : 2#32 ≤ size) (aligned : base &&& (size - 1) = 0) :
    blockSize (blockWord base size) = size ∧ blockBase (blockWord base size) = base := by
  simp only [blockSize, blockBase, blockWord]
  bv_decide

/--
The halves a split makes of an Untyped's word, where the kernel lets it:
each reads back as its half,
an address lies in the block exactly when it lies in one of the halves,
and in no more than one.
-/
theorem split_halves (a x : BitVec 32)
    (pow : (blockSize a / 2) &&& (blockSize a / 2 - 1) = 0) (big : 2#32 ≤ blockSize a / 2)
    (aligned : blockBase a &&& (blockSize a / 2 - 1) = 0) :
    let base := blockBase a
    let half := blockSize a / 2
    blockSize (blockWord base half) = half ∧ blockBase (blockWord base half) = base ∧
    blockSize (blockWord (base + half) half) = half ∧ blockBase (blockWord (base + half) half) = base + half ∧
    (x - base < blockSize a ↔ x - base < half ∨ x - (base + half) < half) ∧
    ¬(x - base < half ∧ x - (base + half) < half) := by
  intro base half
  simp only [base, half, blockSize, blockBase, blockWord] at *
  bv_decide

/-- `napot` is the kernel's check of a split's halves, so `split_halves` covers every split it lets through. -/
theorem napot_split (regionMin : Nat) (base half : BitVec 32) (h : napot regionMin base half = true) :
    half &&& (half - 1) = 0 ∧ base &&& (half - 1) = 0 := by
  simp [napot] at h
  exact ⟨h.1.2, h.2⟩

end Rvuos
