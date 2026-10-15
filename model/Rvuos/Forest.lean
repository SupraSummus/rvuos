/-
Every modelled operation keeps a forest, see "Derivation" in `design/verification.md`.
`Forest` is what `State.wellFormed` checks of a state, said of the children lists:
no node with two lists,
every node in a list and every node with a list filled, in one list at most and there once,
and no node below itself, which a rank that grows down every edge says.
Every outcome a step names is a forest if the state before it was,
a call stopped part way among them, so a revoke or a delete leaves a forest at every step,
and every state `State.wellFormed` passes is a forest.
-/
import Rvuos.Calls

namespace Rvuos

theorem unkey_keys {α : Type} (l : List (Nat × α)) (n : Nat) :
    (unkey l n).map (·.1) = (l.map (·.1)).filter (fun k => decide (k ≠ n)) := by
  induction l with
  | nil => rfl
  | cons x xs ih =>
    by_cases hx : x.1 = n <;> simp_all [unkey]

theorem assoc_of_mem {α : Type} (l : List (Nat × α)) (k : Nat) (v : α)
    (keys : (l.map (·.1)).Nodup) (h : (k, v) ∈ l) : assoc l k = some v := by
  induction l with
  | nil => simp at h
  | cons x xs ih =>
    obtain ⟨x1, x2⟩ := x
    simp only [List.map_cons, List.nodup_cons, List.mem_map] at keys
    rcases List.mem_cons.mp h with h | h
    · simp only [Prod.mk.injEq] at h; obtain ⟨rfl, rfl⟩ := h; simp [assoc]
    · have hne : x1 ≠ k := fun e => keys.1 ⟨(k, v), h, by simp [e]⟩
      simp [assoc, hne, ih keys.2 h]

theorem mem_of_assoc {α : Type} (l : List (Nat × α)) (k : Nat) (v : α) (h : assoc l k = some v) : (k, v) ∈ l := by
  induction l with
  | nil => simp [assoc] at h
  | cons x xs ih =>
    obtain ⟨x1, x2⟩ := x
    simp only [assoc] at h
    split at h
    · simp at h; subst h; rename_i e; subst e; simp
    · exact List.mem_cons_of_mem _ (ih h)

/-- The forest `State.wellFormed` checks, said of the children lists. -/
structure Forest (s : State) : Prop where
  keys : (s.kids.map (·.1)).Nodup
  filled : ∀ p k, k ∈ s.kidsOf p → (s.cap? p).isSome ∧ (s.cap? k).isSome
  nodup : ∀ p, (s.kidsOf p).Nodup
  once : ∀ p q k, k ∈ s.kidsOf p → k ∈ s.kidsOf q → p = q
  rank : ∃ f : Nat → Nat, ∀ p k, k ∈ s.kidsOf p → f p < f k

section
variable {s : State}

theorem State.setKids_keys (s : State) (n : Nat) (l : List Nat) (h : (s.kids.map (·.1)).Nodup) :
    ((s.setKids n l).kids.map (·.1)).Nodup := by
  unfold State.setKids
  have hf : ((unkey s.kids n).map (·.1)).Nodup := by rw [unkey_keys]; exact h.filter _
  split
  · exact hf
  · simp only [List.map_cons, List.nodup_cons]
    refine ⟨?_, hf⟩
    rw [unkey_keys]; simp

/-- A parent is the node whose list holds a node, where no node has two lists and no node is in two. -/
theorem parent?_iff (keys : (s.kids.map (·.1)).Nodup) (once : ∀ p q k, k ∈ s.kidsOf p → k ∈ s.kidsOf q → p = q)
    (n p : Nat) : s.parent? n = some p ↔ n ∈ s.kidsOf p := by
  unfold State.parent?
  constructor
  · intro hp
    cases hf : s.kids.find? (fun q => q.2.contains n) with
    | none => rw [hf] at hp; simp at hp
    | some e =>
      rw [hf] at hp; simp at hp
      have hmem := List.mem_of_find?_eq_some hf
      have hc := List.find?_some hf
      obtain ⟨q, l⟩ := e
      simp at hp hc; subst hp
      simp [State.kidsOf, assoc_of_mem _ _ _ keys hmem, hc]
  · intro hn
    cases hf : s.kids.find? (fun q => q.2.contains n) with
    | none =>
      have hk : s.kidsOf p ≠ [] := List.ne_nil_of_mem hn
      cases ha : assoc s.kids p with
      | none => simp [State.kidsOf, ha] at hn
      | some l =>
        have := List.find?_eq_none.mp hf _ (mem_of_assoc _ _ _ ha)
        simp [State.kidsOf, ha] at hn this
        exact absurd hn this
    | some e =>
      have hmem := List.mem_of_find?_eq_some hf
      have hc := List.find?_some hf
      obtain ⟨q, l⟩ := e
      simp at hc
      have hq : n ∈ s.kidsOf q := by simp [State.kidsOf, assoc_of_mem _ _ _ keys hmem, hc]
      simp [once _ _ _ hq hn]

theorem Forest.parent?_eq (h : Forest s) (n p : Nat) : s.parent? n = some p ↔ n ∈ s.kidsOf p :=
  parent?_iff h.keys h.once n p

theorem Forest.parent?_none (h : Forest s) (n : Nat) : s.parent? n = none ↔ ∀ p, n ∉ s.kidsOf p := by
  constructor
  · intro hn p hp
    rw [(h.parent?_eq n p).mpr hp] at hn
    simp at hn
  · intro hn
    cases hp : s.parent? n with
    | none => rfl
    | some p => exact absurd ((h.parent?_eq n p).mp hp) (hn p)

/-- An empty node is in no list and has none. -/
theorem Forest.empty (h : Forest s) {n : Nat} (hn : s.cap? n = none) : (∀ p, n ∉ s.kidsOf p) ∧ s.kidsOf n = [] := by
  constructor
  · intro p hp; have := (h.filled p n hp).2; simp [hn] at this
  · apply List.eq_nil_iff_forall_not_mem.mpr
    intro k hk; have := (h.filled n k hk).1; simp [hn] at this

theorem Forest.not_self (h : Forest s) {n : Nat} : n ∉ s.kidsOf n := by
  intro hn
  obtain ⟨f, hf⟩ := h.rank
  exact Nat.lt_irrefl _ (hf n n hn)

/-- Filling a node changes no list. -/
theorem Forest.setCap (h : Forest s) (n : Nat) (c : Cap) : Forest (s.setCap n c) where
  keys := h.keys
  filled := by
    intro p k hk
    simp only [State.kidsOf_setCap] at hk
    have := h.filled p k hk
    simp only [State.cap?_setCap]
    constructor <;> split <;> simp_all
  nodup := h.nodup
  once := h.once
  rank := h.rank

/-- Clearing a node in no list and with none. -/
theorem Forest.eraseCap (h : Forest s) {n : Nat} (hroot : ∀ p, n ∉ s.kidsOf p) (hleaf : s.kidsOf n = []) :
    Forest (s.eraseCap n) where
  keys := h.keys
  filled := by
    intro p k hk
    simp only [State.kidsOf_eraseCap] at hk
    have := h.filled p k hk
    simp only [State.cap?_eraseCap]
    have hp : p ≠ n := by intro e; subst e; simp [hleaf] at hk
    have hk' : k ≠ n := by intro e; subst e; exact hroot p hk
    simp [hp, hk', this]
  nodup := h.nodup
  once := h.once
  rank := h.rank

/-- A list that keeps some of a node's children, in any order, once each. -/
theorem Forest.shrink (h : Forest s) {n : Nat} {l : List Nat} (hl : l.Nodup) (hsub : ∀ x ∈ l, x ∈ s.kidsOf n) :
    Forest (s.setKids n l) where
  keys := State.setKids_keys s n l h.keys
  filled := by
    intro p k hk
    simp only [State.kidsOf_setKids, State.cap?_setKids] at hk ⊢
    split at hk
    · subst p; exact h.filled n k (hsub k hk)
    · exact h.filled p k hk
  nodup := by
    intro p
    simp only [State.kidsOf_setKids]
    split
    · exact hl
    · exact h.nodup p
  once := by
    intro p q k hp hq
    simp only [State.kidsOf_setKids] at hp hq
    split at hp <;> split at hq <;> subst_vars
    all_goals first
      | rfl
      | exact h.once _ _ k (hsub k hp) hq
      | exact h.once _ _ k hp (hsub k hq)
      | exact h.once _ _ k hp hq
  rank := by
    obtain ⟨f, hf⟩ := h.rank
    refine ⟨f, fun p k hk => ?_⟩
    simp only [State.kidsOf_setKids] at hk
    split at hk
    · subst p; exact hf n k (hsub k hk)
    · exact hf p k hk

/-- A filled node in no list and with none put in the list of another filled node, among its children. -/
theorem Forest.attach (h : Forest s) {n p : Nat} {l : List Nat}
    (hn : (s.cap? n).isSome) (hroot : ∀ q, n ∉ s.kidsOf q) (hleaf : s.kidsOf n = [])
    (hp : (s.cap? p).isSome) (hpn : p ≠ n) (hl : l.Nodup) (hsub : ∀ x ∈ l, x = n ∨ x ∈ s.kidsOf p) :
    Forest (s.setKids p l) where
  keys := State.setKids_keys s p l h.keys
  filled := by
    intro q k hk
    simp only [State.kidsOf_setKids, State.cap?_setKids] at hk ⊢
    split at hk
    · subst q
      rcases hsub k hk with e | e
      · subst e; exact ⟨hp, hn⟩
      · exact h.filled p k e
    · exact h.filled q k hk
  nodup := by
    intro q
    simp only [State.kidsOf_setKids]
    split
    · exact hl
    · exact h.nodup q
  once := by
    intro q₁ q₂ k h₁ h₂
    simp only [State.kidsOf_setKids] at h₁ h₂
    split at h₁ <;> split at h₂ <;> subst_vars
    · rfl
    · rcases hsub k h₁ with e | e
      · subst e; exact absurd h₂ (hroot _)
      · exact h.once _ _ k e h₂
    · rcases hsub k h₂ with e | e
      · subst e; exact absurd h₁ (hroot _)
      · exact h.once _ _ k h₁ e
    · exact h.once _ _ k h₁ h₂
  rank := by
    obtain ⟨f, hf⟩ := h.rank
    refine ⟨fun x => if x = n then f p + 1 else f x, fun q k hk => ?_⟩
    simp only [State.kidsOf_setKids] at hk
    split at hk
    · subst q
      rcases hsub k hk with e | e
      · subst e; simp [hpn]
      · have : k ≠ n := fun e' => hroot p (e' ▸ e)
        simp [hpn, this, hf p k e]
    · have hqn : q ≠ n := by intro e; subst e; simp [hleaf] at hk
      have hkn : k ≠ n := fun e => hroot q (e ▸ hk)
      simp [hqn, hkn, hf q k hk]

end

/-! The lists a copy, a move and a detach make. -/

theorem mem_insertAfter {b n x : Nat} {l : List Nat} (h : x ∈ State.insertAfter b n l) : x = n ∨ x ∈ l := by
  induction l with
  | nil => simp [State.insertAfter] at h
  | cons y ys ih =>
    simp only [State.insertAfter] at h
    split at h
    · simp at h; rcases h with h | h | h <;> simp [h]
    · simp at h; rcases h with h | h
      · simp [h]
      · rcases ih h with h' | h' <;> simp [h']

theorem nodup_insertAfter {b n : Nat} {l : List Nat} (hl : l.Nodup) (hn : n ∉ l) :
    (State.insertAfter b n l).Nodup := by
  induction l with
  | nil => simp [State.insertAfter]
  | cons y ys ih =>
    simp only [List.nodup_cons, List.mem_cons, not_or] at hl hn
    simp only [State.insertAfter]
    split
    · simp only [List.nodup_cons, List.mem_cons, not_or]
      exact ⟨⟨Ne.symm hn.1, hl.1⟩, hn.2, hl.2⟩
    · simp only [List.nodup_cons]
      refine ⟨fun hy => ?_, ih hl.2 hn.2⟩
      rcases mem_insertAfter hy with e | e
      · exact hn.1 e.symm
      · exact hl.1 e

theorem mem_replaceWith {a x : Nat} {r l : List Nat} (h : x ∈ State.replaceWith a r l) : (x ∈ l ∧ x ≠ a) ∨ x ∈ r := by
  induction l with
  | nil => simp [State.replaceWith] at h
  | cons y ys ih =>
    simp only [State.replaceWith] at h
    split at h
    · rcases List.mem_append.mp h with h | h
      · exact Or.inr h
      · rcases ih h with ⟨h1, h2⟩ | h' <;> simp_all
    · rename_i hy
      rcases List.mem_cons.mp h with h | h
      · subst h; left; simp_all
      · rcases ih h with ⟨h1, h2⟩ | h' <;> simp_all

theorem replaceWith_absent {a : Nat} {r l : List Nat} (h : a ∉ l) : State.replaceWith a r l = l := by
  induction l with
  | nil => rfl
  | cons y ys ih =>
    simp only [List.mem_cons, not_or] at h
    simp only [State.replaceWith]
    have : (y == a) = false := by simp [Ne.symm h.1]
    simp [this, ih h.2]

theorem nodup_replaceWith {a : Nat} {r l : List Nat} (hl : l.Nodup) (hr : r.Nodup) (hd : ∀ x ∈ r, x ∉ l) :
    (State.replaceWith a r l).Nodup := by
  induction l with
  | nil => simp [State.replaceWith]
  | cons y ys ih =>
    simp only [List.nodup_cons] at hl
    simp only [State.replaceWith]
    split
    · rename_i hy
      simp at hy; subst hy
      rw [replaceWith_absent hl.1]
      refine List.nodup_append.mpr ⟨hr, hl.2, fun x hx z hz e => ?_⟩
      subst e; exact hd x hx (List.mem_cons_of_mem _ hz)
    · rename_i hy
      simp only [List.nodup_cons]
      refine ⟨fun hm => ?_, ih hl.2 (fun x hx hm => hd x hx (List.mem_cons_of_mem _ hm))⟩
      rcases mem_replaceWith hm with ⟨h1, _⟩ | h1
      · exact hl.1 h1
      · exact hd y h1 (List.mem_cons_self ..)

theorem replaceWith_single (a b : Nat) (l : List Nat) :
    State.replaceWith a [b] l = l.map (fun x => if x = a then b else x) := by
  induction l with
  | nil => rfl
  | cons y ys ih =>
    simp only [State.replaceWith, List.map_cons]
    by_cases hy : y = a <;> simp [hy, ih]

section
variable {s : State}

/-- `detachAdopt` and the clear after it: a node leaves its parent's list, its children taking its place. -/
theorem Forest.detachErase (h : Forest s) {n p : Nat} (hp : n ∈ s.kidsOf p) :
    Forest ((s.detachAdopt n).eraseCap n) := by
  have hpn : p ≠ n := by intro e; subst e; exact h.not_self hp
  have hpar := (h.parent?_eq n p).mpr hp
  unfold State.detachAdopt
  rw [hpar]
  dsimp only
  obtain ⟨f, hf⟩ := h.rank
  -- The children of the two lists are apart, or the node would be its own parent.
  have hd : ∀ x ∈ s.kidsOf n, x ∉ s.kidsOf p := fun x hx hx' => hpn (h.once _ _ x hx' hx)
  have hkids : ∀ q, ((s.setKids p (State.replaceWith n (s.kidsOf n) (s.kidsOf p))).setKids n []).kidsOf q =
      if q = n then [] else if q = p then State.replaceWith n (s.kidsOf n) (s.kidsOf p) else s.kidsOf q := by
    intro q; simp only [State.kidsOf_setKids]
  have hmem : ∀ x, x ∈ State.replaceWith n (s.kidsOf n) (s.kidsOf p) → (x ∈ s.kidsOf p ∧ x ≠ n) ∨ x ∈ s.kidsOf n :=
    fun x hx => mem_replaceWith hx
  have h2 : Forest ((s.setKids p (State.replaceWith n (s.kidsOf n) (s.kidsOf p))).setKids n []) := {
    keys := State.setKids_keys _ _ _ (State.setKids_keys _ _ _ h.keys)
    filled := by
      intro q k hk
      rw [hkids] at hk
      simp only [State.cap?_setKids]
      split at hk
      · simp at hk
      · split at hk
        · subst q
          rcases hmem k hk with ⟨e, _⟩ | e
          · exact h.filled p k e
          · exact ⟨(h.filled p n hp).1, (h.filled n k e).2⟩
        · exact h.filled q k hk
    nodup := by
      intro q
      rw [hkids]
      split
      · simp
      · split
        · exact nodup_replaceWith (h.nodup p) (h.nodup n) hd
        · exact h.nodup q
    once := by
      intro q₁ q₂ k h₁ h₂
      rw [hkids] at h₁ h₂
      split at h₁
      · simp at h₁
      split at h₂
      · simp at h₂
      rename_i hn₁ hn₂
      split at h₁ <;> split at h₂ <;> subst_vars
      · rfl
      · rcases hmem k h₁ with ⟨e, _⟩ | e
        · exact h.once _ _ k e h₂
        · exact absurd (h.once _ _ k e h₂) (Ne.symm hn₂)
      · rcases hmem k h₂ with ⟨e, _⟩ | e
        · exact h.once _ _ k h₁ e
        · exact absurd (h.once _ _ k e h₁) (Ne.symm hn₁)
      · exact h.once _ _ k h₁ h₂
    rank := by
      refine ⟨f, fun q k hk => ?_⟩
      rw [hkids] at hk
      split at hk
      · simp at hk
      · split at hk
        · subst q
          rcases hmem k hk with ⟨e, _⟩ | e
          · exact hf p k e
          · exact Nat.lt_trans (hf p n hp) (hf n k e)
        · exact hf q k hk }
  refine h2.eraseCap (fun q hq => ?_) (by rw [hkids]; simp)
  rw [hkids] at hq
  split at hq
  · simp at hq
  · split at hq
    · rcases hmem n hq with ⟨_, e⟩ | e
      · exact e rfl
      · exact h.not_self e
    · rename_i _ hqp
      exact hqp (h.once _ _ n hq hp)


/-- What a move leaves in each list: the source's children at the destination, and the destination where the source stood. -/
theorem Forest.kidsOf_move (h : Forest s) {src dst : Nat} {c : Cap} (hsrc : s.cap? src = some c) (hdst : s.cap? dst = none)
    (q : Nat) : (s.move src dst).kidsOf q =
      if q = dst then s.kidsOf src else if q = src then [] else (s.kidsOf q).map (fun x => if x = src then dst else x) := by
  have hne : dst ≠ src := by intro e; subst e; simp [hsrc] at hdst
  unfold State.move
  simp only [hsrc]
  rw [State.kidsOf_setKids, State.kidsOf_setKids]
  by_cases hq : q = dst
  · simp [hq]
  by_cases hqs : q = src
  · simp [hqs]
  simp only [hq, hqs, ↓reduceIte]
  cases hpar : s.parent? src with
  | none =>
    have : src ∉ s.kidsOf q := fun hm => (h.parent?_none src).mp hpar q hm
    simp only [State.kidsOf_setCap, State.kidsOf_eraseCap]
    rw [← replaceWith_single, replaceWith_absent this]
  | some p =>
    dsimp only
    rw [State.kidsOf_setKids]
    by_cases hqp : q = p
    · subst hqp; simp [replaceWith_single]
    · have : src ∉ s.kidsOf q := fun hm => hqp (h.once _ _ _ hm ((h.parent?_eq src p).mp hpar))
      simp only [hqp, ↓reduceIte, State.kidsOf_setCap, State.kidsOf_eraseCap]
      rw [← replaceWith_single, replaceWith_absent this]

/-- `cap_move`: the node at the destination, where the source stood. -/
theorem Forest.move (h : Forest s) {src dst : Nat} {c : Cap} (hsrc : s.cap? src = some c) (hdst : s.cap? dst = none) :
    Forest (s.move src dst) := by
  have hne : dst ≠ src := by intro e; subst e; simp [hsrc] at hdst
  obtain ⟨hroot, hleaf⟩ := h.empty hdst
  obtain ⟨f, hf⟩ := h.rank
  have hk := h.kidsOf_move hsrc hdst
  have hc := fun k => State.cap?_move s src dst k c hsrc
  -- A filled node is neither the destination nor, in a list, the source.
  have filled_ne : ∀ x, (s.cap? x).isSome → x ≠ dst := by intro x hx e; subst e; simp [hdst] at hx
  exact {
    keys := by
      unfold State.move; simp only [hsrc]
      apply State.setKids_keys; apply State.setKids_keys
      split
      · exact h.keys
      · exact State.setKids_keys _ _ _ h.keys
    filled := by
      intro q k hkq
      rw [hk] at hkq
      rw [hc, hc]
      split at hkq
      · subst q
        have := h.filled src k hkq
        have hks : k ≠ src := fun e => h.not_self (e ▸ hkq)
        simp [filled_ne k this.2, hks, this.2]
      · split at hkq
        · simp at hkq
        · rename_i hq hqs
          obtain ⟨x, hx, rfl⟩ := List.mem_map.mp hkq
          have := h.filled q x hx
          simp only [hq, hqs, ↓reduceIte, this.1, true_and]
          by_cases hxs : x = src
          · simp [hxs]
          · simp [hxs, filled_ne x this.2, this.2]
    nodup := by
      intro q
      rw [hk]
      split
      · exact h.nodup src
      · split
        · simp
        · rename_i hq _
          refine List.pairwise_map.mpr ((h.nodup q).imp_of_mem (fun {x y} hx hy hxy e => hxy ?_))
          have hxd := filled_ne x (h.filled q x hx).2
          have hyd := filled_ne y (h.filled q y hy).2
          by_cases hxs : x = src <;> by_cases hys : y = src <;> simp_all
    once := by
      intro q₁ q₂ k h₁ h₂
      rw [hk] at h₁ h₂
      -- What a list at a node other than the two holds: a child of that node, renamed.
      have back : ∀ q, q ≠ dst → q ≠ src → k ∈ (s.kidsOf q).map (fun x => if x = src then dst else x) →
          (k = dst ∧ src ∈ s.kidsOf q) ∨ (k ≠ dst ∧ k ∈ s.kidsOf q) := by
        intro q _ _ hm
        obtain ⟨x, hx, rfl⟩ := List.mem_map.mp hm
        by_cases hxs : x = src
        · subst hxs; simp [hx]
        · simp [hxs, filled_ne x (h.filled q x hx).2, hx]
      split at h₁ <;> split at h₂
      · subst_vars; rfl
      · split at h₂
        · simp at h₂
        rename_i hq₂d hq₂s
        have hkd := filled_ne k (h.filled src k h₁).2
        rcases back q₂ hq₂d hq₂s h₂ with ⟨e, _⟩ | ⟨_, e⟩
        · exact absurd e hkd
        · exact absurd (h.once _ _ k h₁ e).symm hq₂s
      · split at h₁
        · simp at h₁
        rename_i hq₁d _ hq₁s
        have hkd := filled_ne k (h.filled src k h₂).2
        rcases back q₁ hq₁d hq₁s h₁ with ⟨e, _⟩ | ⟨_, e⟩
        · exact absurd e hkd
        · exact absurd (h.once _ _ k h₂ e).symm hq₁s
      · split at h₁
        · simp at h₁
        split at h₂
        · simp at h₂
        rename_i hq₁d hq₂d hq₁s hq₂s
        rcases back q₁ hq₁d hq₁s h₁ with ⟨e₁, m₁⟩ | ⟨e₁, m₁⟩ <;>
          rcases back q₂ hq₂d hq₂s h₂ with ⟨e₂, m₂⟩ | ⟨e₂, m₂⟩
        · exact h.once _ _ _ m₁ m₂
        · exact absurd e₁ e₂
        · exact absurd e₂ e₁
        · exact h.once _ _ _ m₁ m₂
    rank := by
      refine ⟨fun x => if x = dst then f src else f x, fun q k hkq => ?_⟩
      rw [hk] at hkq
      split at hkq
      · subst q
        have := filled_ne k (h.filled src k hkq).2
        simp [this, hf src k hkq]
      · split at hkq
        · simp at hkq
        · rename_i hq hqs
          obtain ⟨x, hx, rfl⟩ := List.mem_map.mp hkq
          by_cases hxs : x = src
          · subst hxs; simp [hq, hf q x hx]
          · simp [hq, hxs, filled_ne x (h.filled q x hx).2, hf q x hx] }


/-- `store` below a filled node: an empty node filled, at the front of the parent's list. -/
theorem Forest.store (h : Forest s) {n p : Nat} (c : Cap) (hn : s.cap? n = none) (hp : (s.cap? p).isSome) :
    Forest (store s n c (some p)) := by
  obtain ⟨hroot, hleaf⟩ := h.empty hn
  have hpn : p ≠ n := by intro e; subst e; simp [hn] at hp
  refine (h.setCap n c).attach (by simp [State.cap?_setCap]) (by simpa [State.kidsOf_setCap] using hroot)
    (by simpa [State.kidsOf_setCap] using hleaf) (by simp [State.cap?_setCap, hpn, hp]) hpn ?_ (by simp)
  simp only [State.kidsOf_setCap, List.nodup_cons]
  exact ⟨hroot p, h.nodup p⟩

/-- A copy: an empty node filled, right after a filled node in its parent's list, or a root. -/
theorem Forest.beside (h : Forest s) {n b : Nat} (c : Cap) (hn : s.cap? n = none) :
    Forest ((s.setCap n c).attachBeside b n) := by
  obtain ⟨hroot, hleaf⟩ := h.empty hn
  unfold State.attachBeside
  have hpar : (s.setCap n c).parent? b = s.parent? b := rfl
  rw [hpar]
  split
  · exact h.setCap n c
  · rename_i p hp
    have hbp := (h.parent?_eq b p).mp hp
    have hpf := (h.filled p b hbp).1
    have hpn : p ≠ n := by intro e; subst e; simp [hn] at hpf
    refine (h.setCap n c).attach (by simp [State.cap?_setCap]) (by simpa [State.kidsOf_setCap] using hroot)
      (by simpa [State.kidsOf_setCap] using hleaf) (by simp [State.cap?_setCap, hpn, hpf]) hpn ?_ ?_
    · simp only [State.kidsOf_setCap]
      exact nodup_insertAfter (h.nodup p) (hroot p)
    · intro x hx
      simp only [State.kidsOf_setCap] at hx
      exact mem_insertAfter hx

end

/-- `cap_delete` of a root, a step a child: a forest at every step, the node a root with the list it is given. -/
theorem deleteRoot_forest (n : Nat) (r : List Nat) (ks : List Nat) :
    ∀ (s : State) (acc : List Outcome) (o : Outcome), Forest s → ks = s.kidsOf n → (∀ q, n ∉ s.kidsOf q) →
      o ∈ deleteRoot n r ks s acc → o ∈ acc ∨ Forest o.st := by
  induction ks with
  | nil =>
    intro s acc o h hks hroot ho
    simp [deleteRoot] at ho
    rcases ho with ho | ho
    · exact Or.inl ho
    · subst ho; exact Or.inr (h.eraseCap hroot hks.symm)
  | cons x xs ih =>
    intro s acc o h hks hroot ho
    have hsub : ∀ y ∈ xs, y ∈ s.kidsOf n := fun y hy => hks ▸ List.mem_cons_of_mem _ hy
    have hnd : xs.Nodup := by have := h.nodup n; rw [← hks] at this; exact (List.nodup_cons.mp this).2
    have h' := h.shrink hnd hsub
    have hroot' : ∀ q, n ∉ (s.setKids n xs).kidsOf q := by
      intro q hq
      rw [State.kidsOf_setKids] at hq
      split at hq
      · exact h.not_self (hsub n hq)
      · exact hroot q hq
    cases xs with
    | nil =>
      simp [deleteRoot] at ho
      rcases ho with ho | ho
      · exact Or.inl ho
      · subst ho; exact Or.inr (h'.eraseCap hroot' (by simp [State.kidsOf_setKids]))
    | cons k ks =>
      simp only [deleteRoot] at ho
      rcases ih _ _ o h' (by simp [State.kidsOf_setKids]) hroot' ho with h'' | h''
      · simp at h''
        rcases h'' with h'' | h''
        · exact Or.inl h''
        · subst h''; exact Or.inr h'
      · exact Or.inr h''

/-- `cap_revoke_below`'s loop: a forest at every step. -/
theorem revokeLoop_forest (m : Machine) (t slot root : Nat) (r : List Nat) :
    ∀ (fuel : Nat) (s : State) (acc outs : List Outcome), Forest s →
      revokeLoop m t slot root r fuel s acc = some outs → ∀ o ∈ outs, o ∈ acc ∨ Forest o.st := by
  intro fuel
  induction fuel with
  | zero => intro s acc outs _ h; simp [revokeLoop] at h
  | succ fuel ih =>
    intro s acc outs hf h o ho
    unfold revokeLoop at h
    split at h
    · simp at h; subst h; simp at ho
      rcases ho with ho | ho
      · exact Or.inl ho
      · subst ho; exact Or.inr hf
    rename_i n _ hk
    have hn : n ∈ s.kidsOf root := by rw [hk]; simp
    have hf' := hf.detachErase hn
    dsimp only at h
    split at h
    · simp at h
    split at h
    · simp at h
    -- Whether the revoke took the slot it was made through, and whether the root has children left.
    repeat' (split at h)
    all_goals first
      | (simp only [Option.some.injEq] at h; subst h; simp at ho
         rcases ho with ho | ho
         · exact Or.inl ho
         · subst ho; exact Or.inr hf')
      | (rcases ih _ _ _ hf' h o ho with h' | h'
         · first
             | exact Or.inl h'
             | (simp at h'
                rcases h' with h' | h'
                · exact Or.inl h'
                · subst h'; exact Or.inr hf')
         · exact Or.inr h')

/-! Each handler, in every outcome it names. -/

section
variable {m : Machine} {s : State}

theorem capDelete_forest (hf : Forest s) {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capDelete s table r = .ok (some outs)) : ∀ o ∈ outs, Forest o.st := by
  intro o ho
  rcases capDelete_ok h with rfl | ⟨n, ⟨p, hp, rfl⟩ | ⟨hp, rfl⟩⟩
  · rw [List.mem_singleton] at ho; subst ho; exact hf
  · rw [List.mem_singleton] at ho; subst ho; exact hf.detachErase ((hf.parent?_eq _ p).mp hp)
  · rcases deleteRoot_forest _ r _ s [] o hf rfl ((hf.parent?_none _).mp hp) ho with h' | h'
    · simp at h'
    · exact h'

/-- Every modelled step keeps a forest: every outcome it names, from a forest, a call stopped part way among them. -/
theorem step_forest (m : Machine) (s : State) (e : Event) (outs : List Outcome) (hf : Forest s)
    (h : step m s e = some outs) : ∀ o ∈ outs, Forest o.st := by
  intro o ho
  have one : ∀ {x : State} {k : Ret}, Forest x → o ∈ [(⟨x, k⟩ : Outcome)] → Forest o.st := by
    intro x k hx ho; rw [List.mem_singleton] at ho; subst ho; exact hx
  rcases step_cases h with ⟨_, rfl⟩ | ⟨t, op, r, slot, node, c, rfl, -, hc, hh⟩
  · exact one hf ho
  rcases hh with ⟨_, -, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
  · rcases opCaptable_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
    · obtain ⟨_, _, _, -, hdst, rfl⟩ := capCopy_ok hh
      exact one (hf.beside _ hdst) ho
    · obtain ⟨_, _, _, hl, hdst, rfl⟩ := capCopy_ok hh
      exact one (hf.store _ hdst (by simp [(slotLookup_held hl).2])) ho
    · obtain ⟨_, _, _, hl, hdst, rfl⟩ := capMove_ok hh
      exact one (hf.move (slotLookup_held hl).2 hdst) ho
    · exact capDelete_forest hf hh o ho
    · obtain ⟨_, _, hh⟩ := capRevoke_ok hh
      rcases revokeLoop_forest m t _ _ r _ s [] _ hf hh o ho with h' | h'
      · simp at h'
      · exact h'
  · obtain ⟨_, -, -, -, hdst, rfl⟩ := opFrame_ok hh
    exact one (hf.store _ hdst (by simp [hc])) ho
  · rcases opUntyped_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩
    · obtain ⟨_, _, hdst, rfl⟩ := untypedRetype_ok hh
      exact one (hf.store _ hdst (by simp [hc])) ho
    · obtain ⟨lo, hi, hlo, hhi, hne, -, rfl⟩ := untypedSplit_ok hh
      have hul : node ≠ lo := by intro e; subst e; simp [hc] at hlo
      refine one ((hf.store _ hlo (by simp [hc])).store _ ?_ ?_) ho
      · simp [cap?_store, Ne.symm hne, hhi]
      · simp [cap?_store, hul, hc]

end

/-! What `model-check` checks of every state is a forest. -/

/-- Two lists of a list with no element twice share an element only where they are one entry. -/
theorem key_of_shared (L : List (Nat × List Nat)) (keys : (L.map (·.1)).Nodup) (all : (L.flatMap (·.2)).Nodup)
    {p q k : Nat} {lp lq : List Nat} (hp : (p, lp) ∈ L) (hq : (q, lq) ∈ L) (kp : k ∈ lp) (kq : k ∈ lq) : p = q := by
  induction L with
  | nil => simp at hp
  | cons e L ih =>
    obtain ⟨a, la⟩ := e
    simp only [List.map_cons, List.nodup_cons, List.flatMap_cons, List.nodup_append] at keys all
    rcases List.mem_cons.mp hp with hp' | hp' <;> rcases List.mem_cons.mp hq with hq' | hq'
    · simp at hp' hq'; rw [hp'.1, hq'.1]
    · simp only [Prod.mk.injEq] at hp'; rw [hp'.2] at kp
      exact absurd rfl (all.2.2 k kp k (List.mem_flatMap.mpr ⟨(q, lq), hq', kq⟩))
    · simp only [Prod.mk.injEq] at hq'; rw [hq'.2] at kq
      exact absurd rfl (all.2.2 k kq k (List.mem_flatMap.mpr ⟨(p, lp), hp', kp⟩))
    · exact ih keys.2 all.2.1 hp' hq'

theorem nodup_of_flatMap (L : List (Nat × List Nat)) (all : (L.flatMap (·.2)).Nodup) {p : Nat} {l : List Nat}
    (h : (p, l) ∈ L) : l.Nodup := by
  induction L with
  | nil => simp at h
  | cons e L ih =>
    simp only [List.flatMap_cons, List.nodup_append] at all
    rcases List.mem_cons.mp h with h | h
    · subst h; exact all.1
    · exact ih all.2.1 h

theorem kidsOf_mem {s : State} {p : Nat} (h : s.kidsOf p ≠ []) : (p, s.kidsOf p) ∈ s.kids := by
  unfold State.kidsOf at h ⊢
  cases ha : assoc s.kids p with
  | none => simp [ha] at h
  | some l => simpa [ha] using mem_of_assoc _ _ _ ha

/-- How many steps up from a node to its root, within `fuel`. -/
def State.depth (s : State) (n : Nat) : Nat → Nat
  | 0 => 0
  | fuel + 1 => match s.parent? n with
    | none => 0
    | some p => s.depth p fuel + 1

theorem depth_stable (s : State) : ∀ f n, s.reachesRoot n f = true → s.depth n f = s.depth n (f + 1)
  | 0, n, h => by simp [State.reachesRoot] at h
  | f + 1, n, h => by
    cases hp : s.parent? n with
    | none => simp [State.depth, hp]
    | some p =>
      simp only [State.reachesRoot, hp] at h
      rw [State.depth.eq_2 s n f, State.depth.eq_2 s n (f + 1), hp]
      dsimp only
      rw [depth_stable s f p h]

/-- Every state `State.wellFormed` passes is a forest. -/
theorem wellFormed_forest (s : State) (h : s.wellFormed = true) : Forest s := by
  simp only [State.wellFormed, Bool.and_eq_true, List.all_eq_true, decide_eq_true_eq] at h
  obtain ⟨⟨⟨hkf, hpf⟩, hkeys, hall⟩, hup⟩ := h
  have hne : ∀ p k, k ∈ s.kidsOf p → s.kidsOf p ≠ [] := fun p k hk => List.ne_nil_of_mem hk
  have once : ∀ p q k, k ∈ s.kidsOf p → k ∈ s.kidsOf q → p = q := fun p q k hp hq =>
    key_of_shared s.kids hkeys hall (kidsOf_mem (hne p k hp)) (kidsOf_mem (hne q k hq)) hp hq
  have filled : ∀ p k, k ∈ s.kidsOf p → (s.cap? p).isSome ∧ (s.cap? k).isSome := fun p k hk =>
    ⟨hpf _ (kidsOf_mem (hne p k hk)), hkf k (List.mem_flatMap.mpr ⟨_, kidsOf_mem (hne p k hk), hk⟩)⟩
  refine ⟨hkeys, filled, fun p => ?_, once, ?_⟩
  · by_cases he : s.kidsOf p = []
    · simp [he]
    · exact nodup_of_flatMap s.kids hall (kidsOf_mem he)
  · refine ⟨fun n => s.depth n (s.caps.length + 1), fun p k hk => ?_⟩
    have hpar := (parent?_iff hkeys once k p).mpr hk
    obtain ⟨c, hc⟩ := Option.isSome_iff_exists.mp (filled p k hk).2
    have hr := hup (k, c) (mem_of_assoc _ _ _ hc)
    simp only [State.reachesRoot, hpar] at hr
    show s.depth p (s.caps.length + 1) < s.depth k (s.caps.length + 1)
    rw [State.depth.eq_2 s k, hpar]
    dsimp only
    rw [← depth_stable s _ p hr]
    exact Nat.lt_succ_self _

end Rvuos
