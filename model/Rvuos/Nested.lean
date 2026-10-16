/-
Every modelled step keeps the nesting of "Isolation", see `design/verification.md`: `step_nested`.
The nesting alone does not follow from step to step,
so `Nested` carries with it what it needs of the derivation rule, `Derived`, which `State.derivedOk` checks:
a pool's own node names a pool, so no lookup finds one;
a frame ends within 32 bits, so a carve lies within it;
what may hold kernel objects lies right below an Untyped, if below anything, so below no frame and no copy;
and below what names an object lies only what names one, so nothing that stands for memory lies below a pool's own node.
`Below` is the order the lists make, which `State.liesBelow` searches within as many steps as there are nodes,
and `checked_nested` says every state `model-check` passes holds `Nested`.
The converse, that the bounded search finds every `Below` of a forest, is not proved,
so a step is proved to keep `Nests`, not `nestingOk`.
-/
import Std.Tactic.BVDecide
import Rvuos.Block
import Rvuos.Forest
import Rvuos.Nesting
import Rvuos.Shrink

namespace Rvuos

open State (mayHold)

/-- `k` lies below `a`: up the lists from `k`, `a` at last. -/
inductive Below (s : State) : Nat → Nat → Prop
  | parent {k p : Nat} : k ∈ s.kidsOf p → Below s k p
  | up {k p a : Nat} : k ∈ s.kidsOf p → Below s p a → Below s k a

namespace State

/-- The memory two nodes stand for overlaps, as `nests` asks it. -/
def Overlap (s : State) (cx cy : Cap) : Prop :=
  ∃ bx zx by_ zy, s.range? cx = some (bx, zx) ∧ s.range? cy = some (by_, zy) ∧ bx < by_ + zy ∧ by_ < bx + zx

/-- `nestingOk` with `Below` for `liesBelow`. -/
def Nests (s : State) : Prop :=
  ∀ x y cx cy, s.cap? x = some cx → s.cap? y = some cy → mayHold cx = true → s.Overlap cx cy →
    x = y ∨ Below s x y ∨ Below s y x

end State

/-- `State.derivedOk`, said of the lists. -/
structure Derived (s : State) : Prop where
  pools : ∀ n c, s.cap? n = some c → c.type = capRetyped → (s.obj? c.a).map (·.type) = some capPool
  fits : ∀ n c, s.cap? n = some c → c.type = capFrame → c.a + c.b ≤ 2 ^ 32
  under : ∀ p k cp ck, k ∈ s.kidsOf p → s.cap? p = some cp → s.cap? k = some ck → mayHold ck = true →
    cp.type = capUntyped
  objects : ∀ p k cp ck, k ∈ s.kidsOf p → s.cap? p = some cp → s.cap? k = some ck → capHasObject cp.type = true →
    capHasObject ck.type = true

section
variable {s s' : State}

theorem Below.mono (h : ∀ p k, k ∈ s.kidsOf p → k ∈ s'.kidsOf p) {x y : Nat} (hb : Below s x y) : Below s' x y := by
  induction hb with
  | parent hk => exact .parent (h _ _ hk)
  | up hk _ ih => exact .up (h _ _ hk) ih

/-- What lies below a node is in its list or below one in it. -/
theorem Below.kid {x a : Nat} (hb : Below s x a) : ∃ k, k ∈ s.kidsOf a := by
  induction hb with
  | parent hk => exact ⟨_, hk⟩
  | up _ _ ih => exact ih

theorem State.Overlap.symm {cx cy : Cap} (h : s.Overlap cx cy) : s.Overlap cy cx := by
  obtain ⟨bx, zx, by_, zy, hx, hy, h1, h2⟩ := h
  exact ⟨by_, zy, bx, zx, hy, hx, h2, h1⟩

theorem State.range?_objs (h : s'.objs = s.objs) (c : Cap) : s'.range? c = s.range? c := by
  simp [State.range?, State.obj?, h]

theorem State.overlap_objs (h : s'.objs = s.objs) {cx cy : Cap} : s'.Overlap cx cy ↔ s.Overlap cx cy := by
  simp [State.Overlap, State.range?_objs h]

/-! What the checks `model-check` runs say. -/

theorem liesBelow_go (h : Forest s) (a : Nat) : ∀ f k, State.liesBelow.go s a f k = true → Below s k a
  | 0, k, hg => by simp [State.liesBelow.go] at hg
  | f + 1, k, hg => by
    simp only [State.liesBelow.go] at hg
    split at hg
    · simp at hg
    · rename_i p hp
      have hk := (h.parent?_eq k p).mp hp
      simp only [Bool.or_eq_true, beq_iff_eq] at hg
      rcases hg with rfl | hg
      · exact .parent hk
      · exact .up hk (liesBelow_go h a f p hg)

theorem liesBelow_below (h : Forest s) {n a : Nat} (hb : s.liesBelow n a = true) : Below s n a :=
  liesBelow_go h a _ n hb

/-- Every state `State.nestingOk` passes, of a forest, `Nests` holds of. -/
theorem nestingOk_nests (h : Forest s) (hn : s.nestingOk = true) : s.Nests := by
  intro x y cx cy hx hy hm ho
  simp only [State.nestingOk, List.all_eq_true] at hn
  have := hn (x, cx) (mem_of_assoc _ _ _ hx)
  simp only [hm, Bool.not_true, Bool.false_or, List.all_eq_true] at this
  have hxy := this (y, cy) (mem_of_assoc _ _ _ hy)
  obtain ⟨bx, zx, by_, zy, hrx, hry, h1, h2⟩ := ho
  simp only [State.nests, hrx, hry, Bool.or_eq_true, beq_iff_eq, Bool.not_eq_true', Bool.and_eq_false_iff,
    decide_eq_false_iff_not] at hxy
  rcases hxy with ((hxy | hxy) | hxy) | hxy
  · exact Or.inl hxy
  · omega
  · exact Or.inr (Or.inl (liesBelow_below h hxy))
  · exact Or.inr (Or.inr (liesBelow_below h hxy))

/-- Every state `State.derivedOk` passes `Derived` holds of. -/
theorem derivedOk_derived (hd : s.derivedOk = true) : Derived s := by
  simp only [State.derivedOk, Bool.and_eq_true, List.all_eq_true] at hd
  obtain ⟨hc, hk⟩ := hd
  have node : ∀ n c, s.cap? n = some c → _ := fun n c h => hc (n, c) (mem_of_assoc _ _ _ h)
  have edge : ∀ p k cp ck, k ∈ s.kidsOf p → s.cap? p = some cp → s.cap? k = some ck →
      (!mayHold ck || cp.type == capUntyped) && (!capHasObject cp.type || capHasObject ck.type) = true := by
    intro p k cp ck hkp hp hk'
    have := hk (p, s.kidsOf p) (kidsOf_mem (List.ne_nil_of_mem hkp)) k hkp
    simpa [hp, hk'] using this
  refine ⟨fun n c h ht => ?_, fun n c h ht => ?_, fun p k cp ck hkp hp hk' hm => ?_, fun p k cp ck hkp hp hk' ho => ?_⟩
  · have := (node n c h).1; simp [ht] at this; simpa using this
  · have := (node n c h).2; simp [ht] at this; exact this
  · have := edge p k cp ck hkp hp hk'; simp [hm] at this; exact this.1
  · have := edge p k cp ck hkp hp hk'; simp [ho] at this; exact this.2

/-! What lies below what, by the derivation rule. -/

/-- Everything above memory that may hold kernel objects is an Untyped. -/
theorem Derived.below_untyped (hd : Derived s) (hf : Forest s) {x a : Nat} (hb : Below s x a) :
    ∀ cx, s.cap? x = some cx → mayHold cx = true → ∃ ca, s.cap? a = some ca ∧ ca.type = capUntyped := by
  induction hb with
  | parent hk =>
    intro cx hx hm
    obtain ⟨cp, hp⟩ := Option.isSome_iff_exists.mp (hf.filled _ _ hk).1
    exact ⟨cp, hp, hd.under _ _ _ _ hk hp hx hm⟩
  | up hk _ ih =>
    intro cx hx hm
    obtain ⟨cp, hp⟩ := Option.isSome_iff_exists.mp (hf.filled _ _ hk).1
    exact ih cp hp (by simp [mayHold, hd.under _ _ _ _ hk hp hx hm])

/-- Below a node that names an object lies only a node that names one. -/
theorem Derived.below_object (hd : Derived s) (hf : Forest s) {x a : Nat} (hb : Below s x a) :
    ∀ ca, s.cap? a = some ca → capHasObject ca.type = true → ∃ cx, s.cap? x = some cx ∧ capHasObject cx.type = true := by
  induction hb with
  | parent hk =>
    intro ca ha ho
    obtain ⟨ck, hk'⟩ := Option.isSome_iff_exists.mp (hf.filled _ _ hk).2
    exact ⟨ck, hk', hd.objects _ _ _ _ hk ha hk' ho⟩
  | up hk _ ih =>
    intro ca ha ho
    obtain ⟨cp, hp, hpo⟩ := ih ca ha ho
    obtain ⟨ck, hk'⟩ := Option.isSome_iff_exists.mp (hf.filled _ _ hk).2
    exact ⟨ck, hk', hd.objects _ _ _ _ hk hp hk' hpo⟩

/-- Nothing that stands for memory lies below a pool's own node. -/
theorem Derived.not_below_pool (hd : Derived s) (hf : Forest s) {x a : Nat} (hb : Below s x a) {cx ca : Cap}
    (hx : s.cap? x = some cx) (ha : s.cap? a = some ca) (hat : ca.type = capRetyped) (hr : (s.range? cx).isSome) :
    False := by
  have hao : capHasObject ca.type = true := by rw [hat]; decide
  obtain ⟨cx', hx', hxo⟩ := hd.below_object hf hb ca ha hao
  rw [hx] at hx'; cases hx'
  -- What both names an object and stands for memory is a pool's own node, which lies right below an Untyped.
  have hxt : cx.type = capRetyped := by
    simp only [State.range?] at hr
    simp only [capHasObject] at hxo
    repeat' split at hr
    all_goals simp_all
  have hm : mayHold cx = true := by simp [mayHold, hxt]
  cases hb with
  | parent hk => have := hd.under _ _ _ _ hk ha hx hm; rw [this] at hat; exact absurd hat (by decide)
  | up hk hb' =>
    obtain ⟨cp, hp⟩ := Option.isSome_iff_exists.mp (hf.filled _ _ hk).1
    have hpt := hd.under _ _ _ _ hk hp hx hm
    obtain ⟨cp', hp', hpo⟩ := hd.below_object hf hb' ca ha hao
    rw [hp] at hp'; cases hp'
    simp [hpt, capHasObject] at hpo

/-- What the proofs carry from state to state. -/
structure Nested (s : State) : Prop where
  forest : Forest s
  derived : Derived s
  nests : s.Nests

/-- What lies below a node is in a list. -/
theorem Below.first {x a : Nat} (hb : Below s x a) : ∃ q, x ∈ s.kidsOf q := by
  cases hb with
  | parent hk => exact ⟨_, hk⟩
  | up hk _ => exact ⟨_, hk⟩

end

section
variable {s s' : State}

/--
A change that fills no node and keeps each node's words,
and puts a node in a list only below where it lay,
keeps the nesting where what lay below another and may meet it still does.
-/
theorem Nested.shrink (h : Nested s) (hf' : Forest s') (hobjs : s'.objs = s.objs) (hs : Shrinks s s')
    (hedge : ∀ p k, k ∈ s'.kidsOf p → Below s k p)
    (hup : ∀ x y cx cy, s'.cap? x = some cx → s'.cap? y = some cy → (mayHold cx = true ∨ mayHold cy = true) →
      s'.Overlap cx cy → Below s x y → Below s' x y) : Nested s' := by
  have hobj : ∀ a, s'.obj? a = s.obj? a := fun a => by simp [State.obj?, hobjs]
  refine ⟨hf', ⟨fun n c hc ht => ?_, fun n c hc ht => ?_, fun p k cp ck hk hp hk' hm => ?_,
    fun p k cp ck hk hp hk' ho => ?_⟩, fun x y cx cy hx hy hm ho => ?_⟩
  · rw [hobj]; exact h.derived.pools n c (hs n c hc) ht
  · exact h.derived.fits n c (hs n c hc) ht
  · obtain ⟨ca, ha, hat⟩ := h.derived.below_untyped h.forest (hedge p k hk) ck (hs k ck hk') hm
    rw [hs p cp hp] at ha; cases ha; exact hat
  · obtain ⟨cx, hx, hxo⟩ := h.derived.below_object h.forest (hedge p k hk) cp (hs p cp hp) ho
    rw [hs k ck hk'] at hx; cases hx; exact hxo
  · rcases h.nests x y cx cy (hs x cx hx) (hs y cy hy) hm ((State.overlap_objs hobjs).mp ho) with e | hb | hb
    · exact Or.inl e
    · exact Or.inr (Or.inl (hup x y cx cy hx hy (Or.inl hm) ho hb))
    · exact Or.inr (Or.inr (hup y x cy cx hy hx (Or.inr hm) ho.symm hb))

theorem mem_replaceWith_of_mem {a x : Nat} {r l : List Nat} (h : x ∈ l) (hx : x ≠ a) : x ∈ State.replaceWith a r l := by
  induction l with
  | nil => simp at h
  | cons y ys ih =>
    simp only [State.replaceWith]
    rcases List.mem_cons.mp h with rfl | h
    · simp [hx]
    · split <;> simp [ih h]

theorem mem_replaceWith_of_r {a x : Nat} {r l : List Nat} (h : a ∈ l) (hx : x ∈ r) : x ∈ State.replaceWith a r l := by
  induction l with
  | nil => simp at h
  | cons y ys ih =>
    simp only [State.replaceWith]
    rcases List.mem_cons.mp h with rfl | h
    · simp [hx]
    · split <;> simp [ih h]

theorem kidsOf_detachErase (h : Forest s) {n p : Nat} (hp : n ∈ s.kidsOf p) (q : Nat) :
    ((s.detachAdopt n).eraseCap n).kidsOf q =
      if q = n then [] else if q = p then State.replaceWith n (s.kidsOf n) (s.kidsOf p) else s.kidsOf q := by
  unfold State.detachAdopt
  rw [(h.parent?_eq n p).mpr hp]
  simp only [State.kidsOf_eraseCap, State.kidsOf_setKids]

theorem objs_detachErase (s : State) (n : Nat) : ((s.detachAdopt n).eraseCap n).objs = s.objs := by
  unfold State.detachAdopt; split <;> rfl

/-- After a detach, what lay below another but the node still does; what lay below the node, below where it lay. -/
theorem below_detachErase (h : Forest s) {n p : Nat} (hp : n ∈ s.kidsOf p) {x a : Nat} (hb : Below s x a) (ha : a ≠ n) :
    (x ≠ n → Below ((s.detachAdopt n).eraseCap n) x a) ∧
    (x = n → ∀ j ∈ s.kidsOf n, Below ((s.detachAdopt n).eraseCap n) j a) := by
  have hpn : p ≠ n := by intro e; subst e; exact h.not_self hp
  have k1 : ∀ q k, k ∈ s.kidsOf q → q ≠ n → k ≠ n → k ∈ ((s.detachAdopt n).eraseCap n).kidsOf q := by
    intro q k hk hq hkn
    rw [kidsOf_detachErase h hp]
    simp only [hq, ↓reduceIte]
    split
    · subst q; exact mem_replaceWith_of_mem hk hkn
    · exact hk
  have k2 : ∀ j ∈ s.kidsOf n, j ∈ ((s.detachAdopt n).eraseCap n).kidsOf p := by
    intro j hj
    rw [kidsOf_detachErase h hp]
    simp only [hpn, ↓reduceIte]
    exact mem_replaceWith_of_r hp hj
  induction hb with
  | @parent k q hk =>
    refine ⟨fun hkn => .parent (k1 _ _ hk ha hkn), fun hkn => ?_⟩
    subst hkn
    have := h.once _ _ _ hk hp; subst this
    exact fun j hj => .parent (k2 j hj)
  | @up k q a' hk _ ih =>
    obtain ⟨ih1, ih2⟩ := ih ha
    refine ⟨fun hkn => ?_, fun hkn => ?_⟩
    · by_cases hq : q = n
      · exact ih2 hq k (hq ▸ hk)
      · exact .up (k1 _ _ hk hq hkn) (ih1 hq)
    · subst hkn
      have := h.once _ _ _ hk hp; subst this
      exact fun j hj => .up (k2 j hj) (ih1 hpn)

/-- A delete of a node that has a parent, and a step of a revoke: its children take its place. -/
theorem Nested.detachErase (h : Nested s) {n p : Nat} (hp : n ∈ s.kidsOf p) :
    Nested ((s.detachAdopt n).eraseCap n) := by
  have hf := h.forest
  refine h.shrink (hf.detachErase hp) (objs_detachErase s n) (revoke_step_shrinks s n) (fun q k hk => ?_)
    (fun x y cx cy hx hy _ _ hb => ?_)
  · rw [kidsOf_detachErase hf hp] at hk
    split at hk
    · simp at hk
    split at hk
    · subst q
      rcases mem_replaceWith hk with ⟨hk, _⟩ | hk
      · exact .parent hk
      · exact .up hk (.parent hp)
    · exact .parent hk
  · have hxn : x ≠ n := by intro e; subst e; simp [State.cap?_eraseCap] at hx
    have hyn : y ≠ n := by intro e; subst e; simp [State.cap?_eraseCap] at hy
    exact (below_detachErase hf hp hb hyn).1 hxn

/-- A node in no list lies below nothing. -/
theorem not_below_root (hroot : ∀ q, n ∉ s.kidsOf q) {a : Nat} (hb : Below s n a) : False := by
  obtain ⟨q, hq⟩ := hb.first; exact hroot q hq

/-- The last step of a delete of a root: the root, in no list and with none, goes. -/
theorem Nested.eraseRoot (h : Nested s) {n : Nat} (hroot : ∀ q, n ∉ s.kidsOf q) (hleaf : s.kidsOf n = []) :
    Nested (s.eraseCap n) :=
  h.shrink (h.forest.eraseCap hroot hleaf) rfl (s.eraseCap_shrinks n) (fun _ _ hk => .parent hk)
    (fun _ _ _ _ _ _ _ _ hb => Below.mono (s := s) (s' := s.eraseCap n) (fun _ _ hk => hk) hb)

/-- A delete of a root: what lay right below it becomes roots, and it goes; what lay below another still does. -/
theorem Nested.deleteRoot (h : Nested s) {n : Nat} (hroot : ∀ q, n ∉ s.kidsOf q) :
    Nested ((s.setKids n []).eraseCap n) := by
  have hf := h.forest
  have hf' : Forest ((s.setKids n []).eraseCap n) := by
    refine (hf.shrink (n := n) (l := []) List.nodup_nil (by simp)).eraseCap (fun q => ?_)
      (by simp [State.kidsOf_setKids])
    rw [State.kidsOf_setKids]
    split
    · simp
    · exact hroot q
  have hkids : ∀ q, ((s.setKids n []).eraseCap n).kidsOf q = if q = n then [] else s.kidsOf q := by
    intro q; rw [State.kidsOf_eraseCap, State.kidsOf_setKids]
  have keep : ∀ x a, Below s x a → a ≠ n → Below ((s.setKids n []).eraseCap n) x a := by
    intro x a hb
    induction hb with
    | parent hk => exact fun ha => .parent (by rw [hkids]; simp [ha, hk])
    | @up k q a' hk hb ih =>
      intro ha
      have hq : q ≠ n := by intro e; subst e; exact not_below_root hroot hb
      exact .up (by rw [hkids]; simp [hq, hk]) (ih ha)
  refine h.shrink hf' rfl ((s.setKids_shrinks n []).trans (State.eraseCap_shrinks _ n)) (fun q k hk => ?_)
    (fun x y cx cy hx hy _ _ hb => ?_)
  · rw [hkids] at hk
    split at hk
    · simp at hk
    · exact .parent hk
  · have hyn : y ≠ n := by intro e; subst e; simp [State.cap?_eraseCap] at hy
    exact keep x y hb hyn

end

/-- `cap_revoke_below`'s loop: the nesting at every step. -/
theorem revokeLoop_nested (m : Machine) (t slot root : Nat) (r : List Nat) :
    ∀ (fuel : Nat) (s : State) (acc outs : List Outcome), Nested s →
      revokeLoop m t slot root r fuel s acc = some outs → ∀ o ∈ outs, o ∈ acc ∨ Nested o.st := by
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

/-! Steps that fill a node. -/

section
variable {s : State}

theorem kidsOf_store (s : State) (n p q : Nat) (c : Cap) :
    (store s n c (some p)).kidsOf q = if q = p then n :: s.kidsOf p else s.kidsOf q := by
  simp [store, State.attach, State.kidsOf_setKids, State.kidsOf_setCap]

/--
A node filled below a filled one:
where it may meet memory that may hold kernel objects, its memory lies within that one's,
and nothing below that one meets it.
-/
theorem Nested.store (h : Nested s) {n p : Nat} {c cp : Cap} (hn : s.cap? n = none) (hp : s.cap? p = some cp)
    (hnot : c.type ≠ capRetyped) (hfit : c.type = capFrame → c.a + c.b ≤ 2 ^ 32)
    (hunder : mayHold c = true → cp.type = capUntyped)
    (hobj : capHasObject cp.type = true → capHasObject c.type = true)
    (hwithin : ∀ cy, s.Overlap c cy → s.Overlap cp cy)
    (hbelow : ∀ x cx, Below s x p → s.cap? x = some cx → (mayHold cx = true ∨ mayHold c = true) → ¬ s.Overlap cx c) :
    Nested (store s n c (some p)) := by
  have hf := h.forest
  obtain ⟨hroot, hleaf⟩ := hf.empty hn
  have hpn : p ≠ n := by intro e; subst e; simp [hn] at hp
  have hcap : ∀ k, (Rvuos.store s n c (some p)).cap? k = if k = n then some c else s.cap? k := fun k => cap?_store ..
  have mono : ∀ {x y}, Below s x y → Below (Rvuos.store s n c (some p)) x y := fun hb =>
    hb.mono fun q k hk => by rw [kidsOf_store]; split <;> simp_all
  have hnp : n ∈ (Rvuos.store s n c (some p)).kidsOf p := by rw [kidsOf_store]; simp
  have hov : ∀ cx cy, (Rvuos.store s n c (some p)).Overlap cx cy ↔ s.Overlap cx cy := fun _ _ => Iff.rfl
  -- An edge of the new state is an old one, or the new node's.
  have hedge : ∀ q k, k ∈ (Rvuos.store s n c (some p)).kidsOf q → (q = p ∧ k = n) ∨ (k ∈ s.kidsOf q ∧ k ≠ n ∧ q ≠ n) := by
    intro q k hk
    rw [kidsOf_store] at hk
    split at hk
    · subst q
      rcases List.mem_cons.mp hk with rfl | hk
      · exact Or.inl ⟨rfl, rfl⟩
      · exact Or.inr ⟨hk, fun e => hroot p (e ▸ hk), hpn⟩
    · exact Or.inr ⟨hk, fun e => hroot q (e ▸ hk), fun e => by subst e; simp [hleaf] at hk⟩
  refine ⟨hf.store c hn (by simp [hp]), ⟨fun k ck hk ht => ?_, fun k ck hk ht => ?_,
    fun q k cq ck hk hq hk' hm => ?_, fun q k cq ck hk hq hk' ho => ?_⟩, fun x y cx cy hx hy hm ho => ?_⟩
  · rw [hcap] at hk; split at hk
    · cases hk; exact absurd ht hnot
    · exact h.derived.pools k ck hk ht
  · rw [hcap] at hk; split at hk
    · cases hk; exact hfit ht
    · exact h.derived.fits k ck hk ht
  · rw [hcap] at hq hk'
    rcases hedge q k hk with ⟨rfl, rfl⟩ | ⟨hk, hkn, hqn⟩
    · simp only [hpn, ↓reduceIte, hp, Option.some.injEq] at hq hk'; subst hq hk'; exact hunder hm
    · simp only [hkn, hqn, ↓reduceIte] at hq hk'; exact h.derived.under q k cq ck hk hq hk' hm
  · rw [hcap] at hq hk'
    rcases hedge q k hk with ⟨rfl, rfl⟩ | ⟨hk, hkn, hqn⟩
    · simp only [hpn, ↓reduceIte, hp, Option.some.injEq] at hq hk'; subst hq hk'; exact hobj ho
    · simp only [hkn, hqn, ↓reduceIte] at hq hk'; exact h.derived.objects q k cq ck hk hq hk' ho
  · rw [hov] at ho
    rw [hcap] at hx hy
    by_cases hxn : x = n <;> by_cases hyn : y = n
    · exact Or.inl (hxn.trans hyn.symm)
    · simp only [hxn, hyn, ↓reduceIte, Option.some.injEq] at hx hy
      subst hx hxn
      rcases h.nests p y cp cy hp hy (by simp [State.mayHold, hunder hm]) (hwithin cy ho) with e | hb | hb
      · subst e; exact Or.inr (Or.inl (.parent hnp))
      · exact Or.inr (Or.inl (.up hnp (mono hb)))
      · exact absurd ho.symm (hbelow y cy hb hy (Or.inr hm))
    · simp only [hxn, hyn, ↓reduceIte, Option.some.injEq] at hx hy
      subst hy hyn
      rcases h.nests x p cx cp hx hp hm (hwithin cx ho.symm).symm with e | hb | hb
      · subst e; exact Or.inr (Or.inr (.parent hnp))
      · exact absurd ho (hbelow x cx hb hx (Or.inl hm))
      · exact Or.inr (Or.inr (.up hnp (mono hb)))
    · simp only [hxn, hyn, ↓reduceIte] at hx hy
      rcases h.nests x y cx cy hx hy hm ho with e | hb | hb
      · exact Or.inl e
      · exact Or.inr (Or.inl (mono hb))
      · exact Or.inr (Or.inr (mono hb))

theorem mem_insertAfter_of_mem {b n x : Nat} {l : List Nat} (h : x ∈ l) : x ∈ State.insertAfter b n l := by
  induction l with
  | nil => simp at h
  | cons y ys ih =>
    simp only [State.insertAfter]
    rcases List.mem_cons.mp h with rfl | h
    · split <;> simp
    · split <;> simp [ih h, h]

theorem mem_insertAfter_self {b n : Nat} {l : List Nat} (h : b ∈ l) : n ∈ State.insertAfter b n l := by
  induction l with
  | nil => simp at h
  | cons y ys ih =>
    simp only [State.insertAfter]
    split
    · simp
    · rename_i hy
      rcases List.mem_cons.mp h with rfl | h
      · simp at hy
      · simp [ih h]

theorem objs_beside (s : State) (n b : Nat) (c : Cap) : ((s.setCap n c).attachBeside b n).objs = s.objs := by
  unfold State.attachBeside; split <;> rfl

theorem kidsOf_beside (s : State) (n b q : Nat) (c : Cap) :
    ((s.setCap n c).attachBeside b n).kidsOf q =
      match s.parent? b with
      | none => s.kidsOf q
      | some p => if q = p then State.insertAfter b n (s.kidsOf p) else s.kidsOf q := by
  unfold State.attachBeside
  have : (s.setCap n c).parent? b = s.parent? b := rfl
  rw [this]
  cases s.parent? b <;> simp [State.kidsOf_setKids, State.kidsOf_setCap]

/--
A copy or a carve of what is no original, which may hold no kernel object:
a node filled beside a filled one, whose memory lies within that one's.
-/
theorem Nested.beside (h : Nested s) {n b : Nat} {c cb : Cap} (hn : s.cap? n = none) (hb : s.cap? b = some cb)
    (ht : c.type = cb.type) (hm : mayHold c = false) (hwithin : ∀ cy, s.Overlap c cy → s.Overlap cb cy)
    (hfit : c.type = capFrame → c.a + c.b ≤ 2 ^ 32) : Nested ((s.setCap n c).attachBeside b n) := by
  have hf := h.forest
  obtain ⟨hroot, hleaf⟩ := hf.empty hn
  have hcap : ∀ k, ((s.setCap n c).attachBeside b n).cap? k = if k = n then some c else s.cap? k := fun k => by
    simp [State.cap?_attachBeside, State.cap?_setCap]
  have hmb : mayHold cb = false := by simpa [State.mayHold, ht] using hm
  have hnot : c.type ≠ capRetyped := by intro e; simp [State.mayHold, e] at hm
  have hbn : b ≠ n := by intro e; subst e; simp [hn] at hb
  -- The new node is beside the old in its parent's list, and every old edge stays.
  have mono : ∀ q k, k ∈ s.kidsOf q → k ∈ ((s.setCap n c).attachBeside b n).kidsOf q := by
    intro q k hk
    rw [kidsOf_beside s]
    split
    · exact hk
    · split
      · subst q; exact mem_insertAfter_of_mem hk
      · exact hk
  have hnew : ∀ q, b ∈ s.kidsOf q → n ∈ ((s.setCap n c).attachBeside b n).kidsOf q := by
    intro q hq
    rw [kidsOf_beside s, (hf.parent?_eq b q).mpr hq]
    simp only [↓reduceIte]
    exact mem_insertAfter_self hq
  have hedge : ∀ q k, k ∈ ((s.setCap n c).attachBeside b n).kidsOf q →
      (k = n ∧ b ∈ s.kidsOf q) ∨ (k ∈ s.kidsOf q ∧ k ≠ n ∧ q ≠ n) := by
    intro q k hk
    have old : k ∈ s.kidsOf q → (k = n ∧ b ∈ s.kidsOf q) ∨ (k ∈ s.kidsOf q ∧ k ≠ n ∧ q ≠ n) := fun hk =>
      Or.inr ⟨hk, fun e => hroot q (e ▸ hk), fun e => by rw [e, hleaf] at hk; simp at hk⟩
    rw [kidsOf_beside s] at hk
    split at hk
    · exact old hk
    · rename_i p hp
      split at hk
      · subst q
        rcases mem_insertAfter hk with rfl | hk
        · exact Or.inl ⟨rfl, (hf.parent?_eq b _).mp hp⟩
        · exact old hk
      · exact old hk
  refine ⟨hf.beside c hn, ⟨fun k ck hk ht' => ?_, fun k ck hk ht' => ?_,
    fun q k cq ck hk hq hk' hm' => ?_, fun q k cq ck hk hq hk' ho => ?_⟩, fun x y cx cy hx hy hmx ho => ?_⟩
  · rw [hcap] at hk; split at hk
    · cases hk; exact absurd ht' hnot
    · rw [State.obj?, objs_beside]; exact h.derived.pools k ck hk ht'
  · rw [hcap] at hk; split at hk
    · cases hk; exact hfit ht'
    · exact h.derived.fits k ck hk ht'
  · rw [hcap] at hq hk'
    rcases hedge q k hk with ⟨hkn, _⟩ | ⟨hk, hkn, hqn⟩
    · simp only [hkn, ↓reduceIte, Option.some.injEq] at hk'; subst hk'; simp [hm] at hm'
    · simp only [hkn, hqn, ↓reduceIte] at hq hk'; exact h.derived.under q k cq ck hk hq hk' hm'
  · rw [hcap] at hq hk'
    rcases hedge q k hk with ⟨hkn, hbq⟩ | ⟨hk, hkn, hqn⟩
    · simp only [hkn, ↓reduceIte, Option.some.injEq] at hk'; subst hk'
      have hqn : q ≠ n := by intro e; rw [e, hleaf] at hbq; simp at hbq
      simp only [hqn, ↓reduceIte] at hq
      rw [ht]; exact h.derived.objects q b cq cb hbq hq hb ho
    · simp only [hkn, hqn, ↓reduceIte] at hq hk'; exact h.derived.objects q k cq ck hk hq hk' ho
  · have ho' : s.Overlap cx cy := (State.overlap_objs (objs_beside s n b c)).mp ho
    rw [hcap] at hx hy
    by_cases hxn : x = n
    · simp only [hxn, ↓reduceIte, Option.some.injEq] at hx; subst hx; simp [hm] at hmx
    simp only [hxn, ↓reduceIte] at hx
    by_cases hyn : y = n
    · simp only [hyn, ↓reduceIte, Option.some.injEq] at hy
      subst hy hyn
      have hob : s.Overlap cx cb := (hwithin cx ho'.symm).symm
      rcases h.nests x b cx cb hx hb hmx hob with e | hb' | hb'
      · subst e; rw [hx] at hb; cases hb; simp [hmx] at hmb
      · obtain ⟨ca, ha, hat⟩ := h.derived.below_untyped hf hb' cx hx hmx
        rw [hb] at ha; cases ha; simp [State.mayHold, hat] at hmb
      · refine Or.inr (Or.inr ?_)
        cases hb' with
        | parent hk => exact .parent (hnew _ hk)
        | up hk hb'' => exact .up (hnew _ hk) (hb''.mono mono)
    · simp only [hyn, ↓reduceIte] at hy
      rcases h.nests x y cx cy hx hy hmx ho' with e | hb' | hb'
      · exact Or.inl e
      · exact Or.inr (Or.inl (hb'.mono mono))
      · exact Or.inr (Or.inr (hb'.mono mono))

theorem objs_move (s : State) (src dst : Nat) : (s.move src dst).objs = s.objs := by
  unfold State.move; split
  · rfl
  · dsimp only; split <;> rfl

/-- A move: the node at the destination, where the source stood, with its words and its children. -/
theorem Nested.move (h : Nested s) {src dst : Nat} {c : Cap} (hsrc : s.cap? src = some c) (hdst : s.cap? dst = none) :
    Nested (s.move src dst) := by
  have hf := h.forest
  have hne : dst ≠ src := by intro e; subst e; simp [hsrc] at hdst
  obtain ⟨hroot, hleaf⟩ := hf.empty hdst
  have hcap := fun k => State.cap?_move s src dst k c hsrc
  have hkids := hf.kidsOf_move hsrc hdst
  -- Where each node of the state before is in the state after.
  let σ : Nat → Nat := fun x => if x = src then dst else x
  have hσ : ∀ x, x ≠ dst → (s.move src dst).cap? (σ x) = s.cap? x := by
    intro x hx
    by_cases hxs : x = src
    · subst hxs; simp [σ, hcap, hsrc]
    · simp [σ, hxs, hcap, hx]
  have hpre : ∀ x' cx, (s.move src dst).cap? x' = some cx → ∃ x, s.cap? x = some cx ∧ σ x = x' ∧ x ≠ dst := by
    intro x' cx hx
    rw [hcap] at hx
    by_cases h1 : x' = dst
    · simp only [h1, ↓reduceIte, Option.some.injEq] at hx; subst hx h1
      exact ⟨src, hsrc, by simp [σ], Ne.symm hne⟩
    · by_cases h2 : x' = src
      · subst h2; simp [Ne.symm hne] at hx
      · simp only [h1, h2, ↓reduceIte] at hx; exact ⟨x', hx, by simp [σ, h2], h1⟩
  have hfwd : ∀ q k, k ∈ s.kidsOf q → σ k ∈ (s.move src dst).kidsOf (σ q) := by
    intro q k hk
    have hks : k ≠ src → σ k = k := fun e => by simp [σ, e]
    by_cases hq : q = src
    · subst hq
      have hkq : k ≠ q := fun e => hf.not_self (e ▸ hk)
      simp only [σ, ↓reduceIte, hkids, hkq]; exact hk
    · have hqd : q ≠ dst := fun e => by rw [e, hleaf] at hk; simp at hk
      simp only [σ, hq, ↓reduceIte, hkids, hqd, List.mem_map]
      exact ⟨k, hk, rfl⟩
  have hbelow : ∀ {x y}, Below s x y → Below (s.move src dst) (σ x) (σ y) := by
    intro x y hb
    induction hb with
    | parent hk => exact .parent (hfwd _ _ hk)
    | up hk _ ih => exact .up (hfwd _ _ hk) ih
  have hback : ∀ q' k', k' ∈ (s.move src dst).kidsOf q' →
      ∃ q k, k ∈ s.kidsOf q ∧ σ q = q' ∧ σ k = k' ∧ q ≠ dst ∧ k ≠ dst := by
    intro q' k' hk
    rw [hkids] at hk
    have hkd : ∀ k q, k ∈ s.kidsOf q → k ≠ dst := fun k q hk e => hroot q (e ▸ hk)
    split at hk
    · subst q'
      have hks : k' ≠ src := fun e => hf.not_self (e ▸ hk)
      exact ⟨src, k', hk, by simp [σ], by simp [σ, hks], Ne.symm hne, hkd _ _ hk⟩
    · split at hk
      · simp at hk
      · rename_i hqd hqs
        obtain ⟨k, hkq, rfl⟩ := List.mem_map.mp hk
        exact ⟨q', k, hkq, by simp [σ, hqs], rfl, hqd, hkd _ _ hkq⟩
  have hobj : ∀ a, (s.move src dst).obj? a = s.obj? a := fun a => by simp [State.obj?, objs_move]
  refine ⟨hf.move hsrc hdst, ⟨fun k ck hk ht => ?_, fun k ck hk ht => ?_,
    fun q' k' cq ck hk hq hk' hm => ?_, fun q' k' cq ck hk hq hk' ho => ?_⟩, fun x' y' cx cy hx hy hm ho => ?_⟩
  · obtain ⟨x, hx0, -, -⟩ := hpre k ck hk
    rw [hobj]; exact h.derived.pools x ck hx0 ht
  · obtain ⟨x, hx0, -, -⟩ := hpre k ck hk
    exact h.derived.fits x ck hx0 ht
  · obtain ⟨q, k, hkq, rfl, rfl, hqd, hkd⟩ := hback q' k' hk
    rw [hσ q hqd] at hq; rw [hσ k hkd] at hk'
    exact h.derived.under q k cq ck hkq hq hk' hm
  · obtain ⟨q, k, hkq, rfl, rfl, hqd, hkd⟩ := hback q' k' hk
    rw [hσ q hqd] at hq; rw [hσ k hkd] at hk'
    exact h.derived.objects q k cq ck hkq hq hk' ho
  · obtain ⟨x, hx0, rfl, hxd⟩ := hpre x' cx hx
    obtain ⟨y, hy0, rfl, hyd⟩ := hpre y' cy hy
    rcases h.nests x y cx cy hx0 hy0 hm ((State.overlap_objs (objs_move s src dst)).mp ho) with e | hb | hb
    · exact Or.inl (by rw [e])
    · exact Or.inr (Or.inl (hbelow hb))
    · exact Or.inr (Or.inr (hbelow hb))

end

/-! The memory a retype, a split and a carve make, in 33 bits, where no sum of two words wraps. -/

theorem toNat_setWidth33 (x : BitVec 32) : (x.setWidth 33).toNat = x.toNat := by
  rw [BitVec.toNat_setWidth]; exact Nat.mod_eq_of_lt (Nat.lt_trans x.isLt (by decide))

theorem toNat_add33 (x y : BitVec 32) : (x.setWidth 33 + y.setWidth 33).toNat = x.toNat + y.toNat := by
  rw [BitVec.toNat_add, toNat_setWidth33, toNat_setWidth33]
  have := x.isLt; have := y.isLt
  exact Nat.mod_eq_of_lt (by omega)

/-- A block ends within 32 bits. -/
theorem block_fits (a : BitVec 32) : (blockBase a).toNat + (blockSize a).toNat ≤ 2 ^ 32 := by
  have h : (blockBase a).setWidth 33 + (blockSize a).setWidth 33 ≤ 0x100000000#33 := by
    simp only [blockBase, blockSize]; bv_decide
  rw [BitVec.le_def, toNat_add33] at h
  exact h

/-- A half of a split lies within the block, or is empty at zero, where `napot` lets the split through. -/
theorem half_within (a : BitVec 32) (lower : Bool)
    (pow : (blockSize a / 2) &&& (blockSize a / 2 - 1) = 0)
    (aligned : blockBase a &&& (blockSize a / 2 - 1) = 0) :
    let w := blockWord (if lower then blockBase a else blockBase a + blockSize a / 2) (blockSize a / 2)
    ((blockSize w).toNat = 0 ∧ (blockBase w).toNat = 0) ∨
    ((blockBase a).toNat ≤ (blockBase w).toNat ∧
      (blockBase w).toNat + (blockSize w).toNat ≤ (blockBase a).toNat + (blockSize a).toNat) := by
  intro w
  have h : (blockSize w = 0 ∧ blockBase w = 0) ∨
      ((blockBase a).setWidth 33 ≤ (blockBase w).setWidth 33 ∧
       (blockBase w).setWidth 33 + (blockSize w).setWidth 33 ≤ (blockBase a).setWidth 33 + (blockSize a).setWidth 33) := by
    cases lower <;> (simp only [w, blockBase, blockSize, blockWord, Bool.false_eq_true, ↓reduceIte] at *; bv_decide)
  rcases h with ⟨h1, h2⟩ | ⟨h1, h2⟩
  · exact Or.inl ⟨by simp [h1], by simp [h2]⟩
  · rw [BitVec.le_def, toNat_setWidth33, toNat_setWidth33] at h1
    rw [BitVec.le_def, toNat_add33, toNat_add33] at h2
    exact Or.inr ⟨h1, h2⟩

/-- The two halves of a split do not overlap. -/
theorem halves_apart (a : BitVec 32)
    (pow : (blockSize a / 2) &&& (blockSize a / 2 - 1) = 0)
    (aligned : blockBase a &&& (blockSize a / 2 - 1) = 0) :
    let l := blockWord (blockBase a) (blockSize a / 2)
    let h := blockWord (blockBase a + blockSize a / 2) (blockSize a / 2)
    ¬((blockBase l).toNat < (blockBase h).toNat + (blockSize h).toNat ∧
      (blockBase h).toNat < (blockBase l).toNat + (blockSize l).toNat) := by
  intro l h
  have hb : ¬((blockBase l).setWidth 33 < (blockBase h).setWidth 33 + (blockSize h).setWidth 33 ∧
      (blockBase h).setWidth 33 < (blockBase l).setWidth 33 + (blockSize l).setWidth 33) := by
    simp only [l, h, blockBase, blockSize, blockWord] at *; bv_decide
  rw [BitVec.lt_def, BitVec.lt_def, toNat_add33, toNat_add33, toNat_setWidth33, toNat_setWidth33] at hb
  exact hb

section
variable {s : State}

theorem State.range?_untyped (s : State) {c : Cap} (ht : c.type = capUntyped) :
    s.range? c = some ((blockBase (BitVec.ofNat 32 c.a)).toNat, (blockSize (BitVec.ofNat 32 c.a)).toNat) := by
  simp (config := { decide := true }) [State.range?, ht]

theorem State.range?_frame (s : State) {c : Cap} (ht : c.type = capFrame) : s.range? c = some (c.a, c.b) := by
  simp [State.range?, ht]

/-- What overlaps memory within another's, or empty at zero, overlaps that other's. -/
theorem State.Overlap.within {c c' cy : Cap} {b z b' z' : Nat} (hr : s.range? c = some (b, z))
    (hr' : s.range? c' = some (b', z')) (hw : (z' = 0 ∧ b' = 0) ∨ (b ≤ b' ∧ b' + z' ≤ b + z))
    (ho : s.Overlap c' cy) : s.Overlap c cy := by
  obtain ⟨bx, zx, by_, zy, hrx, hry, h1, h2⟩ := ho
  rw [hr'] at hrx; cases hrx
  exact ⟨b, z, by_, zy, hr, hry, by omega, by omega⟩

theorem State.mayHold_frame {c : Cap} (h : c.type = capFrame) : mayHold c = false := by
  unfold State.mayHold; rw [h]; decide

theorem untypedFree_leaf {u : Nat} {c : Cap} (h : untypedFree s u c = true) : s.kidsOf u = [] := by
  simpa [untypedFree, List.isEmpty_iff] using h

theorem not_below_leaf {x u : Nat} (h : s.kidsOf u = []) (hb : Below s x u) : False := by
  obtain ⟨k, hk⟩ := hb.kid; simp [h] at hk

end

/-! Each handler, in every outcome it names. -/

section
variable {m : Machine} {s : State}

theorem one_nested {o : Outcome} {x : State} {k : Ret} (hx : Nested x) (ho : o ∈ [(⟨x, k⟩ : Outcome)]) : Nested o.st := by
  rw [List.mem_singleton] at ho; subst ho; exact hx

theorem capCopy_nested (h : Nested s) {t : Nat} {derive : Bool} {table : Obj} {r : List Nat} {outs : List Outcome}
    (hh : capCopy m s t derive table r = .ok (some outs)) : ∀ o ∈ outs, Nested o.st := by
  intro o ho
  obtain ⟨source, src, dst, hl, hdst, rfl⟩ := capCopy_ok hh
  obtain ⟨-, hsrc⟩ := slotLookup_held hl
  -- A pool's own node names a pool, so no lookup finds one.
  have hnr : src.type ≠ capRetyped := by
    intro e
    have := slotLookup_object hl (by rw [e]; decide)
    rw [h.derived.pools _ _ hsrc e, e] at this
    exact absurd (Option.some.inj this) (by decide)
  simp only [finish] at ho
  refine one_nested ?_ ho
  split
  · rename_i hu'
    have hu : src.type = capUntyped := by simpa using hu'
    have hleaf := untypedFree_leaf (capCopy_untyped hh hl hu).2
    exact h.store hdst hsrc hnr (fun ht => h.derived.fits _ src hsrc ht) (fun _ => hu) id (fun _ ho => ho)
      (fun x _ hb => absurd hb (not_below_leaf hleaf ·))
  · rename_i hu'
    have hu : src.type ≠ capUntyped := by simpa using hu'
    have hm : mayHold { src with rights := src.rights &&& (reg r 3 % 256), orig := false } = false := by
      simp [State.mayHold, hu, hnr]
    unfold Rvuos.place
    split
    · refine h.store hdst hsrc hnr (fun ht => h.derived.fits _ src hsrc ht) (fun e => by simp [hm] at e) id
        (fun _ ho => ho) (fun x cx hb hx hmx => ?_)
      rw [hm] at hmx
      obtain ⟨ca, ha, hat⟩ := h.derived.below_untyped h.forest hb cx hx (by simpa using hmx)
      rw [hsrc] at ha; cases ha; exact absurd hat hu
    · exact h.beside hdst hsrc rfl hm (fun _ ho => ho) (fun ht => h.derived.fits _ src hsrc ht)

theorem capDelete_nested (h : Nested s) {table : Obj} {r : List Nat} {outs : List Outcome}
    (hh : capDelete s table r = .ok (some outs)) : ∀ o ∈ outs, Nested o.st := by
  intro o ho
  rcases capDelete_ok hh with rfl | ⟨n, ⟨p, hp, rfl⟩ | ⟨_, -, hp, rfl⟩⟩
  · exact one_nested h ho
  · exact one_nested (h.detachErase ((h.forest.parent?_eq _ p).mp hp)) ho
  · exact one_nested (h.deleteRoot ((h.forest.parent?_none _).mp hp)) ho

/-- A carve: a frame within the frame, below it. -/
theorem opFrame_nested (h : Nested s) {t op node : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (hc : s.cap? node = some c) (hty : c.type = capFrame)
    (hh : opFrame m s t op node c r = .ok (some outs)) : ∀ o ∈ outs, Nested o.st := by
  intro o ho
  obtain ⟨dst, -, h1, h2, hdst, rfl⟩ := opFrame_ok hh
  have hfit := h.derived.fits _ _ hc hty
  have hbase : (BitVec.ofNat 32 c.a + BitVec.ofNat 32 (reg r 1)).toNat = c.a + reg r 1 := by
    rw [BitVec.toNat_add, BitVec.toNat_ofNat, BitVec.toNat_ofNat,
      Nat.mod_eq_of_lt (show c.a < 2 ^ 32 by omega), Nat.mod_eq_of_lt (show reg r 1 < 2 ^ 32 by omega)]
    exact Nat.mod_eq_of_lt (by omega)
  let cf : Cap := ⟨capFrame, c.rights, 0, (BitVec.ofNat 32 c.a + BitVec.ofNat 32 (reg r 1)).toNat, reg r 2, false⟩
  have hwithin : ∀ cy, s.Overlap cf cy → s.Overlap c cy := by
    intro cy ho
    refine State.Overlap.within (State.range?_frame s hty) (State.range?_frame s rfl) (Or.inr ?_) ho
    simp only [cf, hbase]; omega
  have hfit' : cf.type = capFrame → cf.a + cf.b ≤ 2 ^ 32 := fun _ =>
    show (BitVec.ofNat 32 c.a + BitVec.ofNat 32 (reg r 1)).toNat + reg r 2 ≤ 2 ^ 32 by rw [hbase]; omega
  have hm : mayHold cf = false := State.mayHold_frame rfl
  simp only [finish] at ho
  refine one_nested ?_ ho
  unfold Rvuos.place
  split
  · refine h.store hdst hc (show capFrame ≠ capRetyped by decide) hfit' (fun e => by rw [hm] at e; contradiction)
      (fun e => by rw [hty] at e; simp [capHasObject] at e) hwithin (fun x cx hb hx hmx => ?_)
    rcases hmx with hmx | hmx
    · obtain ⟨ca, ha, hat⟩ := h.derived.below_untyped h.forest hb cx hx hmx
      rw [hc] at ha; cases ha; rw [hty] at hat; exact absurd hat (by decide)
    · rw [hm] at hmx; contradiction
  · exact h.beside hdst hc hty.symm hm hwithin hfit'

/-- A retype into a frame: a frame of the whole block, below the free Untyped. -/
theorem untypedRetype_nested (h : Nested s) {t u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (hc : s.cap? u = some c) (hty : c.type = capUntyped)
    (hh : untypedRetype m s t u c r = .ok (some outs)) : ∀ o ∈ outs, Nested o.st := by
  intro o ho
  obtain ⟨dst, ret, hdst, rfl⟩ := untypedRetype_ok hh
  have hleaf := untypedFree_leaf (untypedRetype_free hh)
  refine one_nested (h.store hdst hc (show capFrame ≠ capRetyped by decide) (fun _ => block_fits _) (fun e => by rw [State.mayHold_frame rfl] at e; contradiction)
    (fun e => by rw [hty] at e; simp [capHasObject] at e) (fun cy ho => ?_)
    (fun x _ hb => absurd hb (not_below_leaf hleaf ·))) ho
  exact State.Overlap.within (State.range?_untyped s hty) (State.range?_frame s rfl) (Or.inr ⟨Nat.le_refl _, Nat.le_refl _⟩) ho

/-- A split: two halves below the free Untyped, apart, each within it. -/
theorem untypedSplit_nested (h : Nested s) {t u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (hc : s.cap? u = some c) (hty : c.type = capUntyped)
    (hh : untypedSplit m s t u c r = .ok (some outs)) : ∀ o ∈ outs, Nested o.st := by
  intro o ho
  obtain ⟨lo, hi, hlo, hhi, hne, hnapot, rfl⟩ := untypedSplit_ok hh
  have hleaf := untypedFree_leaf (untypedSplit_free hh)
  obtain ⟨pow, aligned⟩ := napot_split _ _ _ hnapot
  have hul : u ≠ lo := by intro e; subst e; simp [hc] at hlo
  let a := BitVec.ofNat 32 c.a
  let cl : Cap := ⟨capUntyped, c.rights, 0, (blockWord (blockBase a) (blockSize a / 2)).toNat, 0, true⟩
  let ch : Cap := ⟨capUntyped, c.rights, 0, (blockWord (blockBase a + blockSize a / 2) (blockSize a / 2)).toNat, 0, true⟩
  have within : ∀ (st : State) (lower : Bool) (cw : Cap), cw = (if lower then cl else ch) →
      st.cap? u = some c → ∀ cy, st.Overlap cw cy → st.Overlap c cy := by
    intro st lower cw hcw _ cy ho
    have hw := half_within a lower pow aligned
    refine State.Overlap.within (State.range?_untyped st hty) (State.range?_untyped st (by cases lower <;> simp [hcw, cl, ch]))
      ?_ ho
    cases lower <;> simpa [hcw, cl, ch, a, BitVec.ofNat_toNat] using hw
  have h1 := h.store hlo hc (show capUntyped ≠ capRetyped by decide) (fun e => absurd e (show capUntyped ≠ capFrame by decide)) (fun _ => hty) (fun e => by rw [hty] at e; simp [capHasObject] at e)
    (within s true cl rfl hc) (fun x _ hb => absurd hb (not_below_leaf hleaf ·))
  have hc1 : (Rvuos.store s lo cl (some u)).cap? u = some c := by rw [cap?_store]; simp [hul, hc]
  have hhi1 : (Rvuos.store s lo cl (some u)).cap? hi = none := by rw [cap?_store]; simp [Ne.symm hne, hhi]
  -- Below the Untyped lies the lower half alone, and it does not meet the upper.
  have only : ∀ x a', Below (Rvuos.store s lo cl (some u)) x a' → a' = u → x = lo := by
    intro x a' hb
    induction hb with
    | parent hk =>
      intro e; subst e
      rw [kidsOf_store] at hk; simp [hleaf] at hk; exact hk
    | @up k q _ hk _ ih =>
      intro e; have := ih e; subst this
      rw [kidsOf_store] at hk
      simp only [Ne.symm hul, ↓reduceIte] at hk
      exact absurd hk ((h.forest.empty hlo).2 ▸ List.not_mem_nil)
  have h2 := h1.store hhi1 hc1 (show capUntyped ≠ capRetyped by decide) (fun e => absurd e (show capUntyped ≠ capFrame by decide)) (fun _ => hty)
    (fun e => by rw [hty] at e; simp [capHasObject] at e) (within _ false ch rfl hc1)
    (fun x cx hb hx _ ho => ?_)
  · exact one_nested h2 ho
  · have := only x u hb rfl; subst this
    rw [cap?_store] at hx; simp only [↓reduceIte, Option.some.injEq] at hx; subst hx
    obtain ⟨bx, zx, by_, zy, hrx, hry, o1, o2⟩ := ho
    rw [State.range?_untyped _ rfl] at hrx hry
    simp only [Option.some.injEq, Prod.mk.injEq] at hrx hry
    obtain ⟨rfl, rfl⟩ := hrx; obtain ⟨rfl, rfl⟩ := hry
    have := halves_apart a pow aligned
    simp only [cl, ch, BitVec.ofNat_toNat, BitVec.setWidth_eq] at o1 o2 this
    exact this ⟨o1, o2⟩

/-- Every modelled step keeps the nesting, with what it needs of the derivation rule:
every outcome it names, a call stopped part way among them. -/
theorem step_nested (m : Machine) (s : State) (e : Event) (outs : List Outcome) (h : Nested s)
    (hs : step m s e = some outs) : ∀ o ∈ outs, Nested o.st := by
  intro o ho
  rcases step_cases hs with ⟨_, rfl⟩ | ⟨t, op, r, slot, node, c, rfl, -, hc, hh⟩
  · exact one_nested h ho
  rcases hh with ⟨_, -, hh⟩ | ⟨hty, hh⟩ | ⟨hty, hh⟩
  · rcases opCaptable_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
    · exact capCopy_nested h hh o ho
    · exact capCopy_nested h hh o ho
    · obtain ⟨_, _, _, hl, hdst, rfl⟩ := capMove_ok hh
      exact one_nested (h.move (slotLookup_held hl).2 hdst) ho
    · exact capDelete_nested h hh o ho
    · obtain ⟨_, _, hh⟩ := capRevoke_ok hh
      rcases revokeLoop_nested m t _ _ r _ s [] _ h hh o ho with h' | h'
      · simp at h'
      · exact h'
  · exact opFrame_nested h hc hty hh o ho
  · rcases opUntyped_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩
    · exact untypedRetype_nested h hc hty hh o ho
    · exact untypedSplit_nested h hc hty hh o ho

end

/-- Every state `model-check` passes holds what every step keeps. -/
theorem checked_nested (s : State) (hw : s.wellFormed = true) (hd : s.derivedOk = true) (hn : s.nestingOk = true) :
    Nested s :=
  ⟨wellFormed_forest s hw, derivedOk_derived hd, nestingOk_nests (wellFormed_forest s hw) hn⟩

end Rvuos
