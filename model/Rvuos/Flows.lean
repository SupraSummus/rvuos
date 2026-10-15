/-
Authority only flows, see "Properties" in `design/verification.md`.
Every node a modelled call fills or changes is covered by a capability the caller's tables held before the call:
the same capability with no more rights,
or memory within the memory of a frame or an Untyped, with no more rights,
where an Untyped covers the frames it makes.
It holds in every outcome the model names, a call stopped part way among them,
and of a fault, which changes nothing.
-/
import Std.Tactic.BVDecide
import Rvuos.Block
import Rvuos.Shrink

namespace Rvuos

/-- Whether a frame or an Untyped grants an address, `range_contains` over its base and size. -/
def Cap.holds (c : Cap) (x : BitVec 32) : Prop :=
  if c.type = capUntyped then x - blockBase (BitVec.ofNat 32 c.a) < blockSize (BitVec.ofNat 32 c.a)
  else (x - BitVec.ofNat 32 c.a).toNat < c.b

/--
`covered` in `host/history.c`: `h` grants all that `g` does.
No more rights, and the same capability, or memory no address of which `h` does not grant.
`covered` compares the bases and sizes, which says the same of every block that is not empty;
this compares the addresses, so an empty block lies within any.
-/
def Grants (h g : Cap) : Prop :=
  g.rights &&& h.rights = g.rights ∧
  ({ g with rights := h.rights } = h ∨
   ((g.type = capFrame ∨ g.type = capUntyped) ∧ (h.type = g.type ∨ h.type = capUntyped) ∧
    ∀ x, g.holds x → h.holds x))

/-- Every node `s'` fills, `s` filled with the same words, or a node the caller held covers. -/
def Flows (m : Machine) (s : State) (t : Nat) (s' : State) : Prop :=
  ∀ n c, s'.cap? n = some c → s.cap? n = some c ∨ ∃ n₀ c₀, Held m s t n₀ ∧ s.cap? n₀ = some c₀ ∧ Grants c₀ c

theorem Grants.self (c : Cap) : Grants c c := ⟨Nat.and_self _, Or.inl rfl⟩

theorem Grants.masked (c : Cap) (k : Nat) : Grants c { c with rights := c.rights &&& k } := by
  refine ⟨?_, Or.inl rfl⟩
  show c.rights &&& k &&& c.rights = c.rights &&& k
  rw [Nat.and_assoc, Nat.and_comm k, ← Nat.and_assoc, Nat.and_self]

theorem Shrinks.flows {m : Machine} {s s' : State} {t : Nat} (h : Shrinks s s') : Flows m s t s' :=
  fun n c hc => Or.inl (h n c hc)

/-- A step that fills one node with what a held node covers. -/
theorem flows_fill {m : Machine} {s s' : State} {t n n₀ : Nat} {c c₀ : Cap}
    (hs : ∀ k, s'.cap? k = if k = n then some c else s.cap? k)
    (held : Held m s t n₀) (h₀ : s.cap? n₀ = some c₀) (g : Grants c₀ c) : Flows m s t s' := by
  intro k d hk
  rw [hs] at hk
  split at hk
  · simp at hk; subst hk; exact Or.inr ⟨n₀, c₀, held, h₀, g⟩
  · exact Or.inl hk

theorem flows_store {m : Machine} {s : State} {t n n₀ : Nat} {c c₀ : Cap} {p : Option Nat}
    (held : Held m s t n₀) (h₀ : s.cap? n₀ = some c₀) (g : Grants c₀ c) : Flows m s t (store s n c p) :=
  flows_fill (fun k => cap?_store s n k c p) held h₀ g

/-! The memory a carve, a split and a retype make lies within what made it. -/

theorem carve_within (ca off len cb : Nat) (x : BitVec 32) (h1 : off < cb) (h2 : len ≤ cb - off)
    (hx : (x - (BitVec.ofNat 32 ca + BitVec.ofNat 32 off)).toNat < len) :
    (x - BitVec.ofNat 32 ca).toNat < cb := by
  have e : x - BitVec.ofNat 32 ca = (x - (BitVec.ofNat 32 ca + BitVec.ofNat 32 off)) + BitVec.ofNat 32 off := by
    bv_omega
  rw [e, BitVec.toNat_add]
  have := Nat.mod_le ((x - (BitVec.ofNat 32 ca + BitVec.ofNat 32 off)).toNat + (BitVec.ofNat 32 off).toNat) (2^32)
  have : (BitVec.ofNat 32 off).toNat ≤ off := by simp [Nat.mod_le]
  omega

/-- Each half of a split lies within the block, for every 32-bit word; the empty halves of a block too small for the check too. -/
theorem split_within (a x : BitVec 32)
    (pow : (blockSize a / 2) &&& (blockSize a / 2 - 1) = 0)
    (aligned : blockBase a &&& (blockSize a / 2 - 1) = 0) :
    (x - blockBase (blockWord (blockBase a) (blockSize a / 2)) < blockSize (blockWord (blockBase a) (blockSize a / 2)) →
      x - blockBase a < blockSize a) ∧
    (x - blockBase (blockWord (blockBase a + blockSize a / 2) (blockSize a / 2)) <
        blockSize (blockWord (blockBase a + blockSize a / 2) (blockSize a / 2)) →
      x - blockBase a < blockSize a) := by
  simp only [blockSize, blockBase, blockWord] at *
  bv_decide

theorem holds_untyped (c : Cap) (x : BitVec 32) (h : c.type = capUntyped) :
    c.holds x ↔ x - blockBase (BitVec.ofNat 32 c.a) < blockSize (BitVec.ofNat 32 c.a) := by
  simp [Cap.holds, h]

theorem holds_frame (c : Cap) (x : BitVec 32) (h : c.type = capFrame) :
    c.holds x ↔ (x - BitVec.ofNat 32 c.a).toNat < c.b := by
  have : ¬(capFrame = capUntyped) := by decide
  simp only [Cap.holds, h, this, ↓reduceIte]

/-! Each handler, in every outcome it names. -/

section
variable {m : Machine} {s : State}

theorem capCopy_flows {t : Nat} {derive : Bool} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capCopy m s t derive table r = .ok (some outs)) : ∀ o ∈ outs, Flows m s t o.st := by
  obtain ⟨source, src, dst, hl, -, rfl⟩ := capCopy_ok h
  obtain ⟨held, hsrc⟩ := slotLookup_held hl
  intro o ho
  rw [List.mem_singleton] at ho; subst ho
  cases derive <;> simp only [Bool.false_eq_true, ↓reduceIte, finish]
  · exact flows_fill (n := dst) (fun k => by simp [State.cap?_attachBeside, State.cap?_setCap])
      held hsrc (Grants.masked _ (reg r 3 % 256))
  · exact flows_store held hsrc (Grants.masked _ (reg r 3 % 256))

theorem capMove_flows {t : Nat} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capMove m s t table r = .ok (some outs)) : ∀ o ∈ outs, Flows m s t o.st := by
  obtain ⟨source, src, dst, hl, -, rfl⟩ := capMove_ok h
  obtain ⟨held, hsrc⟩ := slotLookup_held hl
  intro o ho k d hk
  rw [List.mem_singleton] at ho; subst ho
  simp only [finish, State.cap?_move s source dst k src hsrc] at hk
  split at hk
  · simp at hk; subst hk; exact Or.inr ⟨source, _, held, hsrc, Grants.self _⟩
  · split at hk
    · simp at hk
    · exact Or.inl hk

theorem opFrame_flows {t op node : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (held : Held m s t node) (hc : s.cap? node = some c) (hty : c.type = capFrame)
    (h : opFrame m s t op node c r = .ok (some outs)) : ∀ o ∈ outs, Flows m s t o.st := by
  obtain ⟨_, -, h1, h2, -, rfl⟩ := opFrame_ok h
  intro o ho
  rw [List.mem_singleton] at ho; subst ho
  refine flows_store held hc ⟨Nat.and_self _, Or.inr ⟨Or.inl rfl, Or.inl hty, fun x hx => ?_⟩⟩
  rw [holds_frame _ _ hty]
  rw [holds_frame _ _ rfl] at hx
  simp only [BitVec.ofNat_toNat, BitVec.setWidth_eq] at hx
  exact carve_within c.a (reg r 1) (reg r 2) c.b x h1 h2 hx

theorem untypedRetype_flows {t u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (held : Held m s t u) (hc : s.cap? u = some c) (hty : c.type = capUntyped)
    (h : untypedRetype m s t u c r = .ok (some outs)) : ∀ o ∈ outs, Flows m s t o.st := by
  obtain ⟨_, _, -, rfl⟩ := untypedRetype_ok h
  intro o ho
  rw [List.mem_singleton] at ho; subst ho
  refine flows_store held hc ⟨Nat.and_self _, Or.inr ⟨Or.inl rfl, Or.inr hty, fun x hx => ?_⟩⟩
  rw [holds_untyped _ _ hty, BitVec.lt_def]
  rw [holds_frame _ _ rfl] at hx
  simpa using hx

theorem untypedSplit_flows {t u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (held : Held m s t u) (hc : s.cap? u = some c) (hty : c.type = capUntyped)
    (h : untypedSplit m s t u c r = .ok (some outs)) : ∀ o ∈ outs, Flows m s t o.st := by
  obtain ⟨_, _, -, -, -, hnapot, rfl⟩ := untypedSplit_ok h
  obtain ⟨pow, aligned⟩ := napot_split _ _ _ hnapot
  have within := fun x => split_within (BitVec.ofNat 32 c.a) x pow aligned
  -- Each half grants an address only where the block does.
  have half : ∀ w : BitVec 32, (∀ x, x - blockBase w < blockSize w → x - blockBase (BitVec.ofNat 32 c.a) <
      blockSize (BitVec.ofNat 32 c.a)) → Grants c ⟨capUntyped, c.rights, 0, w.toNat, 0⟩ := by
    intro w hw
    refine ⟨Nat.and_self _, Or.inr ⟨Or.inr rfl, Or.inr hty, fun x hx => ?_⟩⟩
    rw [holds_untyped _ _ hty]
    rw [holds_untyped _ _ rfl] at hx
    simp only [BitVec.ofNat_toNat, BitVec.setWidth_eq] at hx
    exact hw x hx
  intro o ho k d hk
  rw [List.mem_singleton] at ho; subst ho
  simp only [finish, cap?_store] at hk
  split at hk
  · simp at hk; subst hk; exact Or.inr ⟨u, c, held, hc, half _ fun x => (within x).2⟩
  · split at hk
    · simp at hk; subst hk; exact Or.inr ⟨u, c, held, hc, half _ fun x => (within x).1⟩
    · exact Or.inl hk

end

/-- Authority only flows: in every outcome of every step the model names, a fault's among them. -/
theorem step_flows (m : Machine) (s : State) (e : Event) (outs : List Outcome)
    (h : step m s e = some outs) : ∀ o ∈ outs, Flows m s e.thread o.st := by
  rcases step_cases h with ⟨_, rfl⟩ | ⟨t, op, r, slot, node, c, rfl, held, hc, hh⟩
  · intro o ho; simp at ho; subst ho; exact (Shrinks.refl s).flows
  rcases hh with ⟨_, -, hh⟩ | ⟨hty, hh⟩ | ⟨hty, hh⟩
  · rcases opCaptable_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
    · exact capCopy_flows hh
    · exact capCopy_flows hh
    · exact capMove_flows hh
    · exact fun o ho => (capDelete_shrinks hh o ho).flows
    · exact fun o ho => (capRevoke_shrinks hh o ho).flows
  · exact opFrame_flows held hc hty hh
  · rcases opUntyped_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩
    · exact untypedRetype_flows held hc hty hh
    · exact untypedSplit_flows held hc hty hh

end Rvuos
