/-
Every modelled step keeps every node derived from its parent, `derived_from` of "Derivation": `step_parented`.
`Derives` is `derivedFrom` case by case.
`Derives.trans` carries it over a node a delete or a revoke takes, whose children come up to its parent;
it does not hold over a pool's own node, whose children carry rights of their own,
and need not: no delete finds one, as a table holds the ABI's types alone, and a revoke stops at one.
-/
import Rvuos.Nested
import Rvuos.Parents

namespace Rvuos

/-! Rights and bits. -/

theorem sub_self (x : Nat) : sub x x = true := by simp [sub, Nat.and_self]

theorem sub_mask (x y : Nat) : sub (x &&& y) x = true := by
  simp only [sub, beq_iff_eq]
  rw [Nat.and_assoc, Nat.and_comm y x, ← Nat.and_assoc, Nat.and_self]

theorem sub_trans {x y z : Nat} (h₁ : sub x y = true) (h₂ : sub y z = true) : sub x z = true := by
  simp only [sub, beq_iff_eq] at *
  rw [← h₁, Nat.and_assoc, h₂]

theorem sub_right {x y : Nat} (h : sub x y = true) (hx : x &&& rightR ≠ 0) : y &&& rightR ≠ 0 := by
  simp only [sub, beq_iff_eq] at h
  intro hy; apply hx; rw [← h, Nat.and_assoc, hy, Nat.and_zero]

/-! The kernel's types. -/

/-- The type codes as numbers, for `omega`. -/
macro "unfold_types" : tactic => `(tactic| try simp only [capNone, capFrame, capPool, capCaptable, capProcess,
  capThread, capPrint, capNotification, capUntyped, capIrqLine, capIrq, capClock, capTime, capHalt, capTest, capRouteLine,
  capRoute, capInstalled, capRetyped, capBound, capHosted, capSignalled, capWatched, capRouted] at *)

theorem kernelType_untyped {t : Nat} : kernelType t = capUntyped ↔ t = capUntyped := by
  unfold kernelType
  repeat' split
  all_goals unfold_types
  all_goals first | omega | (constructor <;> intro <;> first | trivial | omega)

theorem kernelType_frame {t : Nat} : kernelType t = capFrame ↔ t = capFrame ∨ t = capInstalled ∨ t = capRouted := by
  unfold kernelType
  repeat' split
  all_goals unfold_types
  all_goals first | omega | (constructor <;> intro <;> first | trivial | omega)

theorem kernelType_abi {t : Nat} (h : t < capInstalled) : kernelType t = t := by
  unfold kernelType
  repeat' split
  all_goals unfold_types
  all_goals first | omega | (constructor <;> intro <;> first | trivial | omega)

theorem State.range?_frameKind (s : State) {c : Cap} (h : kernelType c.type = capFrame) : s.range? c = some (c.a, c.b) := by
  rcases kernelType_frame.mp h with e | e | e <;> simp [State.range?, e]

/-- `derivedFrom`, case by case. -/
inductive Derives (m : Machine) (s : State) : Cap → Cap → Prop
  /-- What an Untyped made, or what came up to it, within its block. -/
  | untyped {c p : Cap} {b z pb pz : Nat} : p.type = capUntyped → s.range? c = some (b, z) → s.range? p = some (pb, pz) →
      (c.type = capUntyped ∨ kernelType c.type = capFrame ∨ c.type = capRetyped) →
      (sub c.rights p.rights = true ∨ c.type = capRetyped) → within b z pb pz = true → Derives m s c p
  /-- A frame, an installed region or a Route's end within a frame. -/
  | frame {c p : Cap} : kernelType c.type = capFrame → kernelType p.type = capFrame →
      sub c.rights p.rights = true → within c.a c.b p.a p.b = true → Derives m s c p
  /-- A thread's units among those of the capability it was bound through. -/
  | bound {c p : Cap} : c.type = capBound → kernelType p.type = capTime →
      sub c.rights p.rights = true → unitsWithin c.a c.b p.a p.b = true → Derives m s c p
  /-- A range of lines, units or channels within one of its type. -/
  | range {c p : Cap} : kernelType c.type = kernelType p.type →
      (kernelType c.type = capIrqLine ∨ kernelType c.type = capTime ∨ kernelType c.type = capRouteLine) →
      c.type ≠ capBound → sub c.rights p.rights = true → within c.a c.b p.a p.b = true → Derives m s c p
  /-- The same object, or nothing, with no more rights, and for a notification no more bits. -/
  | same {c p : Cap} : kernelType c.type = kernelType p.type → kernelType c.type ≠ capUntyped →
      kernelType c.type ≠ capFrame → kernelType c.type ≠ capIrqLine → kernelType c.type ≠ capTime →
      kernelType c.type ≠ capRouteLine →
      (sub c.rights p.rights = true ∨ p.type = capRetyped) → (namesNothing (kernelType c.type) = true ∨ c.a = p.a) →
      (kernelType c.type ≠ capNotification ∨ sub c.b p.b = true) → Derives m s c p
  /-- The counter's block below a clock that reads. -/
  | clock {c p : Cap} : kernelType c.type = capFrame → kernelType p.type = capClock → p.rights &&& rightR ≠ 0 →
      sub c.rights m.counterRights = true → within c.a c.b m.counterBase m.counterSize = true → Derives m s c p
  /-- A capability to an object below its pool's. -/
  | pool {c p : Cap} : kernelType p.type = capPool → isObjectType (kernelType c.type) = true →
      (s.obj? c.a).map (·.pool) = some p.a → Derives m s c p

private theorem if_yes {α : Type} {q : Prop} [Decidable q] {a b : α} (h : q) : (if q then a else b) = a := by simp [h]

private theorem if_no {α : Type} {q : Prop} [Decidable q] {a b : α} (h : ¬q) : (if q then a else b) = b := by simp [h]

section
variable {m : Machine} {s : State} {c p : Cap}

theorem derives_of (h : derivedFrom m s c p = true) : Derives m s c p := by
  unfold derivedFrom at h
  dsimp only at h
  split at h
  · rename_i h1
    split at h
    · rename_i b z pb pz hrc hrp
      simp only [Bool.and_eq_true, Bool.or_eq_true, beq_iff_eq] at h
      obtain ⟨⟨hty, hr⟩, hw⟩ := h
      refine .untyped h1 hrc hrp ?_ hr hw
      rcases hty with (e | e) | e
      · exact Or.inl (kernelType_untyped.mp e)
      · exact Or.inr (Or.inl e)
      · exact Or.inr (Or.inr e)
    · simp at h
  rename_i h1
  split at h
  · rename_i h2
    split at h
    · rename_i h3
      simp only [Bool.and_eq_true] at h
      exact .frame h3 (h2 ▸ h3) h.1 h.2
    rename_i h3
    split at h
    · rename_i h4
      simp only [Bool.and_eq_true] at h
      have : kernelType c.type = capTime := by rw [h4]; decide
      exact .bound h4 (h2 ▸ this) h.1 h.2
    rename_i h4
    split at h
    · rename_i h5
      simp only [Bool.and_eq_true] at h
      exact .range h2 h5 h4 h.1 h.2
    rename_i h5
    simp only [Bool.and_eq_true, Bool.or_eq_true, beq_iff_eq, bne_iff_ne, ne_eq] at h
    obtain ⟨⟨hr, hn⟩, hb⟩ := h
    have hu : kernelType c.type ≠ capUntyped := by
      rw [h2]; intro e; exact h1 (kernelType_untyped.mp e)
    refine .same h2 hu h3 (fun e => h5 (Or.inl e)) (fun e => h5 (Or.inr (Or.inl e))) (fun e => h5 (Or.inr (Or.inr e)))
      hr hn ?_
    by_cases e : kernelType c.type = capNotification
    · exact Or.inr (hb.resolve_left (fun h' => h' e))
    · exact Or.inl e
  rename_i h2
  split at h
  · rename_i h6
    simp only [Bool.and_eq_true, bne_iff_ne, ne_eq] at h
    exact .clock h6.1 h6.2 h.1.1 h.1.2 h.2
  rename_i h6
  split at h
  · rename_i h7
    simp only [Bool.and_eq_true, beq_iff_eq] at h
    exact .pool h7 h.1 h.2
  · simp at h

/-- The converse: `Derives` says no more than `derivedFrom`, so what is proved of it is what `model-check` checks. -/
theorem derivedFrom_of (h : Derives m s c p) : derivedFrom m s c p = true := by
  unfold derivedFrom
  dsimp only
  have notU : ∀ {q : Cap}, kernelType q.type ≠ capUntyped → q.type ≠ capUntyped :=
    fun hq e => hq (kernelType_untyped.mpr e)
  cases h with
  | untyped hp hrc hrp hty hr hw =>
    rw [if_yes hp, hrc, hrp]
    simp only [Bool.and_eq_true, Bool.or_eq_true, beq_iff_eq, hw, and_true]
    refine ⟨?_, hr⟩
    rcases hty with e | e | e
    · exact Or.inl (Or.inl (kernelType_untyped.mpr e))
    · exact Or.inl (Or.inr e)
    · exact Or.inr e
  | frame hc hp hr hw =>
    rw [if_no (notU (by rw [hp]; decide)), if_yes (hc.trans hp.symm), if_yes hc]
    simp [hr, hw]
  | bound hc hp hr hw =>
    have hct : kernelType c.type = capTime := by rw [hc]; decide
    rw [if_no (notU (by rw [hp]; decide)), if_yes (hct.trans hp.symm), if_no (by rw [hct]; decide), if_yes hc]
    simp [hr, hw]
  | range ht hk hb hr hw =>
    have hcu : kernelType c.type ≠ capUntyped := by rcases hk with e | e | e <;> rw [e] <;> decide
    have hcf : kernelType c.type ≠ capFrame := by rcases hk with e | e | e <;> rw [e] <;> decide
    rw [if_no (notU (ht ▸ hcu)), if_yes ht, if_no hcf, if_no hb, if_yes hk]
    simp [hr, hw]
  | same ht hu hf hi htm hrl hr hn hb =>
    have hb' : c.type ≠ capBound := fun e => htm (by rw [e]; decide)
    rw [if_no (notU (ht ▸ hu)), if_yes ht, if_no hf, if_no hb', if_no (by simp [hi, htm, hrl])]
    simp only [Bool.and_eq_true, Bool.or_eq_true, beq_iff_eq, bne_iff_ne, ne_eq]
    refine ⟨⟨hr, hn⟩, ?_⟩
    rcases hb with e | e
    · exact Or.inl e
    · exact Or.inr e
  | clock hc hp hR hr hw =>
    rw [if_no (notU (by rw [hp]; decide)), if_no (by rw [hc, hp]; decide), if_yes ⟨hc, hp⟩]
    simp only [Bool.and_eq_true, bne_iff_ne]
    exact ⟨⟨hR, hr⟩, hw⟩
  | pool hp ho hpool =>
    have hpo : isObjectType (kernelType p.type) = false := by rw [hp]; decide
    have hne : kernelType c.type ≠ kernelType p.type := fun e => by rw [e] at ho; simp [hpo] at ho
    rw [if_no (notU (by rw [hp]; decide)), if_no hne, if_no (fun e => absurd (e.2.symm.trans hp) (by decide)), if_yes hp]
    simp [ho, hpool]

/-! Ranges within ranges. -/

theorem within_trans {b z nb nz pb pz : Nat} (h₁ : within b z nb nz = true) (h₂ : within nb nz pb pz = true) :
    within b z pb pz = true := by
  simp only [within, Bool.and_eq_true, decide_eq_true_eq] at *; omega

theorem units_trans {f n nf nn pf pn : Nat} (h₁ : unitsWithin f n nf nn = true) (h₂ : unitsWithin nf nn pf pn = true) :
    unitsWithin f n pf pn = true := by
  simp only [unitsWithin, Bool.and_eq_true, decide_eq_true_eq] at *; omega

theorem units_within {f n nf nn pf pn : Nat} (h₁ : unitsWithin f n nf nn = true) (h₂ : within nf nn pf pn = true) :
    unitsWithin f n pf pn = true := by
  simp only [unitsWithin, within, Bool.and_eq_true, decide_eq_true_eq] at *; omega

theorem within_units {f n nf nn pf pn : Nat} (h₁ : within f n nf nn = true) (h₂ : unitsWithin nf nn pf pn = true) :
    within f n pf pn = true := by
  simp only [unitsWithin, within, Bool.and_eq_true, decide_eq_true_eq] at *; omega

/-- Two facts of a node's kernel type that cannot both hold. -/
macro "types_apart" : tactic => `(tactic| (exfalso; (try simp only [isObjectType, namesNothing, Bool.or_eq_true, beq_iff_eq] at *); unfold_types; omega))

/--
A derivation goes over a node that is not a pool's own node:
what was derived from it is derived from its parent, so its children come up to the parent derived.
-/
theorem Derives.trans {k n : Cap} (h₁ : Derives m s k n) (h₂ : Derives m s n p) (hn : n.type ≠ capRetyped) :
    Derives m s k p := by
  have hnu := kernelType_untyped (t := n.type)
  cases h₂ with
  | untyped hp hrn hrp hty hr hw =>
    have hr' := hr.resolve_right hn
    have hkn : kernelType n.type = capUntyped ∨ kernelType n.type = capFrame := by
      rcases hty with e | e | e
      · exact Or.inl (kernelType_untyped.mpr e)
      · exact Or.inr e
      · exact absurd e hn
    cases h₁
    case untyped _ hrk hrn' hty' hr₁ hw₁ =>
      rw [hrn] at hrn'; cases hrn'
      exact .untyped hp hrk hrp hty' (hr₁.imp_left (sub_trans · hr')) (within_trans hw₁ hw)
    case frame hk hnf hr₁ hw₁ =>
      rw [State.range?_frameKind s hnf] at hrn; cases hrn
      exact .untyped hp (State.range?_frameKind s hk) hrp (Or.inr (Or.inl hk)) (Or.inl (sub_trans hr₁ hr')) (within_trans hw₁ hw)
    all_goals types_apart
  | frame hn' hp hr hw =>
    cases h₁
    case frame hk _ hr₁ hw₁ => exact .frame hk hp (sub_trans hr₁ hr) (within_trans hw₁ hw)
    all_goals types_apart
  | bound hnb hp hr hw =>
    have hnt : kernelType n.type = capTime := by rw [hnb]; decide
    cases h₁
    case bound hk _ hr₁ hw₁ => exact .bound hk hp (sub_trans hr₁ hr) (units_trans hw₁ hw)
    case range ht hk hkb hr₁ hw₁ =>
      exact .range (ht.trans (hnt.trans hp.symm)) hk hkb (sub_trans hr₁ hr) (within_units hw₁ hw)
    all_goals types_apart
  | range ht hk' hnb hr hw =>
    cases h₁
    case bound hk hnt hr₁ hw₁ => exact .bound hk (ht ▸ hnt) (sub_trans hr₁ hr) (units_within hw₁ hw)
    case range ht₁ hk _ hr₁ hw₁ => exact .range (ht₁.trans ht) hk ‹_› (sub_trans hr₁ hr) (within_trans hw₁ hw)
    all_goals types_apart
  | same ht hu hf hi htm hrl hr hnn hb =>
    cases h₁
    case same ht₁ hu₁ hf₁ hi₁ htm₁ hrl₁ hr₁ hnn₁ hb₁ =>
      have hr₁' := hr₁.resolve_right hn
      refine .same (ht₁.trans ht) hu₁ hf₁ hi₁ htm₁ hrl₁ (hr.imp_left (sub_trans hr₁' ·)) ?_ ?_
      · rcases hnn₁ with e | e
        · exact Or.inl e
        · rcases hnn with e' | e'
          · exact Or.inl (ht₁ ▸ e')
          · exact Or.inr (e.trans e')
      · rcases hb₁ with e | e
        · exact Or.inl e
        · rcases hb with e' | e'
          · exact Or.inl (fun e'' => e' (ht₁ ▸ e''))
          · exact Or.inr (sub_trans e e')
    case clock hk hnc hR hr₁ hw₁ =>
      have hr' : sub n.rights p.rights = true := hr.resolve_right (fun e => by
        rw [e] at ht; rw [hnc] at ht; exact absurd ht (by decide))
      exact .clock hk (ht ▸ hnc) (sub_right hr' hR) hr₁ hw₁
    case pool hnp ho hpool =>
      have ha : n.a = p.a := hnn.resolve_left (by rw [hnp]; decide)
      exact .pool (ht ▸ hnp) ho (ha ▸ hpool)
    all_goals types_apart
  | clock hn' hp hR hr hw =>
    cases h₁
    case frame hk _ hr₁ hw₁ => exact .clock hk hp hR (sub_trans hr₁ hr) (within_trans hw₁ hw)
    all_goals types_apart
  | pool hp ho hpool =>
    cases h₁
    case same ht₁ _ _ _ _ _ _ hnn₁ _ =>
      have ha : k.a = n.a := hnn₁.resolve_left (by
        rw [ht₁]; revert ho; simp only [isObjectType, namesNothing, Bool.or_eq_true, beq_iff_eq]; unfold_types; omega)
      exact .pool hp (ht₁ ▸ ho) (ha ▸ hpool)
    all_goals types_apart

/-! What a step's changes to a node's words leave of a derivation. -/

theorem State.range?_congr (s : State) {c c' : Cap} (ht : c'.type = c.type) (ha : c'.a = c.a) (hb : c'.b = c.b) :
    s.range? c' = s.range? c := by
  simp [State.range?, ht, ha, hb]

/-- A derivation holds of a node with fewer rights. -/
theorem Derives.narrow (h : Derives m s c p) {c' : Cap} (ht : c'.type = c.type) (ha : c'.a = c.a) (hb : c'.b = c.b)
    (hr : sub c'.rights c.rights = true) : Derives m s c' p := by
  cases h with
  | untyped hp hrc hrp hty hr' hw =>
    exact .untyped hp (by rw [State.range?_congr s ht ha hb, hrc]) hrp (ht ▸ hty)
      (hr'.imp (sub_trans hr ·) (ht.trans ·)) hw
  | frame hc hp hr' hw => exact .frame (ht ▸ hc) hp (sub_trans hr hr') (ha ▸ hb ▸ hw)
  | bound hc hp hr' hw => exact .bound (ht ▸ hc) hp (sub_trans hr hr') (ha ▸ hb ▸ hw)
  | range h1 h2 h3 hr' hw => exact .range (ht ▸ h1) (ht ▸ h2) (ht ▸ h3) (sub_trans hr hr') (ha ▸ hb ▸ hw)
  | same h1 h2 h3 h4 h5 h6 hr' hn hb' =>
    exact .same (ht ▸ h1) (ht ▸ h2) (ht ▸ h3) (ht ▸ h4) (ht ▸ h5) (ht ▸ h6) (hr'.imp_left (sub_trans hr ·))
      (ht ▸ ha ▸ hn) (ht ▸ hb ▸ hb')
  | clock hc hp hR hr' hw => exact .clock (ht ▸ hc) hp hR (sub_trans hr hr') (ha ▸ hb ▸ hw)
  | pool hp ho hpool => exact .pool hp (ht ▸ ho) (ha ▸ hpool)

theorem within_self (b z : Nat) : within b z b z = true := by simp [within]

/-- A derive: a capability of the ABI's types, its rights masked, below itself. -/
theorem Derives.masked (hty : c.type < capInstalled) (x : Nat) : Derives m s { c with rights := c.rights &&& x } c := by
  have hk := kernelType_abi hty
  by_cases hu : c.type = capUntyped
  · have hr := State.range?_untyped s hu
    exact .untyped hu ((State.range?_congr s (c := c) (c' := { c with rights := c.rights &&& x }) rfl rfl rfl).trans hr) hr
      (Or.inl hu) (Or.inl (sub_mask _ _)) (within_self _ _)
  by_cases hf : c.type = capFrame
  · have : kernelType c.type = capFrame := by rw [hf]; decide
    exact .frame this this (sub_mask _ _) (within_self _ _)
  have hb : c.type ≠ capBound := by unfold_types; omega
  by_cases hrg : c.type = capIrqLine ∨ c.type = capTime ∨ c.type = capRouteLine
  · exact .range rfl (by simpa [hk] using hrg) hb (sub_mask _ _) (within_self _ _)
  simp only [not_or] at hrg
  exact .same rfl (by simpa [hk] using hu) (by simpa [hk] using hf) (by simpa [hk] using hrg.1)
    (by simpa [hk] using hrg.2.1) (by simpa [hk] using hrg.2.2) (Or.inl (sub_mask _ _)) (Or.inr rfl) (Or.inr (sub_self _))

/-- An Untyped marked going is the parent it was: the mark lies where its block does not. -/
theorem Derives.markGoing {k : Cap} (h : Derives m s k c) : Derives m s k c.markGoing := by
  unfold Cap.markGoing
  split
  · rename_i hu
    have hku := kernelType_untyped (t := c.type)
    cases h
    case untyped hp hrk hrp hty hr hw =>
      have hg : ({ c with b := untypedGoing } : Cap).type = capUntyped := hu
      exact .untyped hp hrk (by rw [State.range?_untyped s hg, ← hrp, State.range?_untyped s hu]) hty hr hw
    all_goals types_apart
  · exact h

theorem Derives.objs {s' : State} (hobjs : s'.objs = s.objs) (h : Derives m s c p) : Derives m s' c p := by
  have hobj : ∀ a, s'.obj? a = s.obj? a := fun a => by simp [State.obj?, hobjs]
  cases h with
  | untyped hp hrc hrp hty hr hw =>
    exact .untyped hp (by rw [State.range?_objs hobjs, hrc]) (by rw [State.range?_objs hobjs, hrp]) hty hr hw
  | frame h1 h2 h3 h4 => exact .frame h1 h2 h3 h4
  | bound h1 h2 h3 h4 => exact .bound h1 h2 h3 h4
  | range h1 h2 h3 h4 h5 => exact .range h1 h2 h3 h4 h5
  | same h1 h2 h3 h4 h5 h6 h7 h8 h9 => exact .same h1 h2 h3 h4 h5 h6 h7 h8 h9
  | clock h1 h2 h3 h4 h5 => exact .clock h1 h2 h3 h4 h5
  | pool hp ho hpool => exact .pool hp ho (by rw [hobj]; exact hpool)

end

/-- `State.parentsOk`, said of the lists. -/
structure Parented (m : Machine) (s : State) : Prop where
  tables : ∀ o, o ∈ s.objs → o.type = capCaptable → ∀ i, i < o.count → ∀ c, s.cap? (nodeAt o i) = some c →
    c.type < capInstalled
  edges : ∀ p k cp ck, k ∈ s.kidsOf p → s.cap? p = some cp → s.cap? k = some ck → Derives m s ck cp

section
variable {m : Machine} {s : State}

/-- Every state `model-check` passes holds `Parented`. -/
theorem checked_parented (h : s.parentsOk m = true) : Parented m s := by
  simp only [State.parentsOk, State.tablesOk, Bool.and_eq_true, List.all_eq_true] at h
  obtain ⟨ht, hk⟩ := h
  refine ⟨fun o ho hty i hi c hc => ?_, fun p k cp ck hkp hp hk' => derives_of ?_⟩
  · have := ht o ho
    simp only [hty, bne_self_eq_false, Bool.false_or, List.all_eq_true, List.mem_range] at this
    simpa [hc] using this i hi
  · have := hk (p, s.kidsOf p) (kidsOf_mem (List.ne_nil_of_mem hkp)) k hkp
    simpa [hp, hk'] using this

/-- A state with no edge and no node the first had not, and the same objects. -/
theorem Parented.sub {s' : State} (h : Parented m s) (hobjs : s'.objs = s.objs)
    (hk : ∀ q k, k ∈ s'.kidsOf q → k ∈ s.kidsOf q) (hc : ∀ x c, s'.cap? x = some c → s.cap? x = some c) :
    Parented m s' where
  tables o ho hty i hi c hx := h.tables o (hobjs ▸ ho) hty i hi c (hc _ _ hx)
  edges p k cp ck hkp hp hk' := (h.edges p k cp ck (hk _ _ hkp) (hc _ _ hp) (hc _ _ hk')).objs hobjs

theorem State.cap?_detachAdopt (s : State) (n k : Nat) : (s.detachAdopt n).cap? k = s.cap? k := by
  unfold State.detachAdopt; split <;> simp [State.cap?_setKids]

/-- An edge between two nodes that were there before, where one empty node is filled. -/
theorem Parented.old (h : Parented m s) (hf : Forest s) {n q k : Nat} {c cq ck : Cap} (hn : s.cap? n = none)
    (hk : k ∈ s.kidsOf q) (hq : (if q = n then some c else s.cap? q) = some cq)
    (hk' : (if k = n then some c else s.cap? k) = some ck) : Derives m s ck cq := by
  obtain ⟨hroot, hleaf⟩ := hf.empty hn
  have hkn : k ≠ n := fun e => hroot q (e ▸ hk)
  have hqn : q ≠ n := fun e => by rw [e, hleaf] at hk; simp at hk
  rw [if_no hkn] at hk'; rw [if_no hqn] at hq
  exact h.edges _ _ _ _ hk hq hk'

/-- `store` below a filled node: an empty node filled with the ABI's type, derived from the parent. -/
theorem Parented.store (h : Parented m s) (hf : Forest s) {n p : Nat} {c cp : Cap} (hn : s.cap? n = none)
    (hp : s.cap? p = some cp) (hty : c.type < capInstalled) (hd : Derives m s c cp) :
    Parented m (store s n c (some p)) := by
  have hpn : p ≠ n := by intro e; subst e; simp [hn] at hp
  have hobjs : (Rvuos.store s n c (some p)).objs = s.objs := rfl
  refine ⟨fun o ho ht i hi c' hx => ?_, fun q k cq ck hk hq hk' => ?_⟩
  · rw [cap?_store] at hx
    split at hx
    · cases hx; exact hty
    · exact h.tables o ho ht i hi c' hx
  · rw [cap?_store] at hq hk'
    rw [kidsOf_store] at hk
    apply Derives.objs hobjs
    split at hk
    · rename_i hqp
      rcases List.mem_cons.mp hk with hkn | hk
      · rw [if_yes hkn] at hk'; cases hk'
        rw [if_no (hqp ▸ hpn), hqp, hp] at hq; cases hq; exact hd
      · exact h.old hf hn (hqp ▸ hk) hq hk'
    · exact h.old hf hn hk hq hk'

/-- A copy: an empty node filled with the ABI's type beside a node, derived from that node's parent. -/
theorem Parented.beside (h : Parented m s) (hf : Forest s) {n b : Nat} {c : Cap} (hn : s.cap? n = none)
    (hty : c.type < capInstalled) (hd : ∀ q cq, b ∈ s.kidsOf q → s.cap? q = some cq → Derives m s c cq) :
    Parented m ((s.setCap n c).attachBeside b n) := by
  obtain ⟨hroot, -⟩ := hf.empty hn
  have hobjs := objs_beside s n b c
  have hcap : ∀ x, ((s.setCap n c).attachBeside b n).cap? x = if x = n then some c else s.cap? x := fun x => by
    rw [State.cap?_attachBeside, State.cap?_setCap]
  refine ⟨fun o ho ht i hi c' hx => ?_, fun q k cq ck hk hq hk' => ?_⟩
  · rw [hcap] at hx; split at hx
    · cases hx; exact hty
    · exact h.tables o (hobjs ▸ ho) ht i hi c' hx
  · rw [hcap] at hq hk'
    rw [kidsOf_beside] at hk
    apply Derives.objs hobjs
    cases hpar : s.parent? b with
    | none => rw [hpar] at hk; exact h.old hf hn hk hq hk'
    | some p₀ =>
      rw [hpar] at hk
      dsimp only at hk
      split at hk
      · rename_i hqp
        rcases mem_insertAfter hk with hkn | hk
        · rw [if_yes hkn] at hk'; cases hk'
          have hbp : b ∈ s.kidsOf p₀ := (hf.parent?_eq b p₀).mp hpar
          have hqn : q ≠ n := fun e => by
            have := (hf.filled p₀ b hbp).1; rw [← hqp, e, hn] at this; simp at this
          rw [if_no hqn] at hq
          exact hd q cq (hqp ▸ hbp) hq
        · exact h.old hf hn (hqp ▸ hk) hq hk'
      · exact h.old hf hn hk hq hk'

/-- `cap_move`: the node at an empty one, of the ABI's type, where it stood. -/
theorem Parented.move (h : Parented m s) (hf : Forest s) {src dst : Nat} {c : Cap} (hsrc : s.cap? src = some c)
    (hdst : s.cap? dst = none) (hty : c.type < capInstalled) : Parented m (s.move src dst) := by
  have hcap := fun k => State.cap?_move s src dst k c hsrc
  have hobjs := objs_move s src dst
  have filled_ne : ∀ x, (s.cap? x).isSome → x ≠ dst := by intro x hx e; subst e; simp [hdst] at hx
  refine ⟨fun o ho ht i hi c' hx => ?_, fun q k cq ck hk hq hk' => ?_⟩
  · rw [hcap] at hx; split at hx
    · cases hx; exact hty
    · split at hx
      · simp at hx
      · exact h.tables o (hobjs ▸ ho) ht i hi c' hx
  · apply Derives.objs hobjs
    rw [hf.kidsOf_move hsrc hdst] at hk
    rw [hcap] at hq hk'
    split at hk
    · rename_i hqd
      have hkd := filled_ne k (hf.filled src k hk).2
      have hks : k ≠ src := fun e => hf.not_self (e ▸ hk)
      rw [if_yes hqd] at hq; cases hq
      rw [if_no hkd, if_no hks] at hk'
      exact h.edges _ _ _ _ hk hsrc hk'
    · split at hk
      · simp at hk
      · rename_i hqd hqs
        rw [if_no hqd, if_no hqs] at hq
        obtain ⟨x, hx, rfl⟩ := List.mem_map.mp hk
        by_cases hxs : x = src
        · subst hxs; simp only [↓reduceIte, Option.some.injEq] at hk'; subst hk'
          exact h.edges _ _ _ _ hx hq hsrc
        · have hxd := filled_ne x (hf.filled q x hx).2
          rw [if_no hxs, if_no hxd, if_no hxs] at hk'
          exact h.edges _ _ _ _ hx hq hk'

/-- `detachAdopt` and the clear after it: the node's children come up to its parent, derived from it. -/
theorem Parented.detachErase (h : Parented m s) (hf : Forest s) {n p : Nat} {cn : Cap} (hp : n ∈ s.kidsOf p)
    (hcn : s.cap? n = some cn) (hnr : cn.type ≠ capRetyped) : Parented m ((s.detachAdopt n).eraseCap n) := by
  have hobjs := objs_detachErase s n
  have hcap : ∀ x, ((s.detachAdopt n).eraseCap n).cap? x = if x = n then none else s.cap? x := fun x => by
    rw [State.cap?_eraseCap, State.cap?_detachAdopt]
  refine ⟨fun o ho ht i hi c' hx => ?_, fun q k cq ck hk hq hk' => ?_⟩
  · rw [hcap] at hx; split at hx
    · simp at hx
    · exact h.tables o (hobjs ▸ ho) ht i hi c' hx
  · apply Derives.objs hobjs
    rw [kidsOf_detachErase hf hp] at hk
    rw [hcap] at hq hk'
    split at hk
    · simp at hk
    rename_i hqn
    rw [if_no hqn] at hq
    have hkn : k ≠ n := fun e => by rw [if_yes e] at hk'; simp at hk'
    rw [if_no hkn] at hk'
    split at hk
    · rename_i hqp
      rw [hqp] at hq
      rcases mem_replaceWith hk with ⟨hk, -⟩ | hk
      · exact h.edges _ _ _ _ hk hq hk'
      · exact (h.edges _ _ _ _ hk hcn hk').trans (h.edges _ _ _ _ hp hq hcn) hnr
    · exact h.edges _ _ _ _ hk hq hk'

/-- A root marked going, its children derived from it as they were. -/
theorem Parented.markGoing (h : Parented m s) {n : Nat} {c : Cap} (hc : s.cap? n = some c)
    (hroot : ∀ q, n ∉ s.kidsOf q) : Parented m (s.setCap n c.markGoing) := by
  have hty : c.markGoing.type = c.type := by unfold Cap.markGoing; split <;> rfl
  refine ⟨fun o ho ht i hi c' hx => ?_, fun q k cq ck hk hq hk' => ?_⟩
  · rw [State.cap?_setCap] at hx; split at hx
    · rename_i e; cases hx; rw [hty]; exact h.tables o ho ht i hi c (by rw [e]; exact hc)
    · exact h.tables o ho ht i hi c' hx
  · rw [State.kidsOf_setCap] at hk
    rw [State.cap?_setCap] at hq hk'
    have hkn : k ≠ n := fun e => hroot q (e ▸ hk)
    rw [if_no hkn] at hk'
    refine Derives.objs (s := s) rfl ?_
    split at hq
    · rename_i e; cases hq; subst e; exact (h.edges _ _ _ _ hk hc hk').markGoing
    · exact h.edges _ _ _ _ hk hq hk'

/-- `cap_delete` of a root, a step a child: what is left of it at every step, no edge new. -/
theorem deleteRoot_parented (n : Nat) (r : List Nat) (ks : List Nat) :
    ∀ (s : State) (acc : List Outcome) (o : Outcome), Parented m s → ks = s.kidsOf n →
      o ∈ deleteRoot n r ks s acc → o ∈ acc ∨ Parented m o.st := by
  have erase : ∀ s : State, Parented m s → Parented m (s.eraseCap n) := fun s h =>
    h.sub rfl (fun q k hk => by rwa [State.kidsOf_eraseCap] at hk)
      (fun x c hx => by rw [State.cap?_eraseCap] at hx; split at hx; · simp at hx
                        exact hx)
  have shrink : ∀ (s : State) (l : List Nat), Parented m s → (∀ y ∈ l, y ∈ s.kidsOf n) → Parented m (s.setKids n l) :=
    fun s l h hl => h.sub rfl
      (fun q k hk => by
        rw [State.kidsOf_setKids] at hk
        split at hk
        · rename_i e; rw [e]; exact hl k hk
        · exact hk)
      (fun x c hx => by rwa [State.cap?_setKids] at hx)
  induction ks with
  | nil =>
    intro s acc o h hks ho
    simp [deleteRoot] at ho
    rcases ho with ho | ho
    · exact Or.inl ho
    · subst ho; exact Or.inr (erase s h)
  | cons x xs ih =>
    intro s acc o h hks ho
    have hsub : ∀ y ∈ xs, y ∈ s.kidsOf n := fun y hy => hks ▸ List.mem_cons_of_mem _ hy
    have h' := shrink s xs h hsub
    cases xs with
    | nil =>
      simp [deleteRoot] at ho
      rcases ho with ho | ho
      · exact Or.inl ho
      · subst ho; exact Or.inr (erase _ h')
    | cons k ks =>
      simp only [deleteRoot] at ho
      rcases ih _ _ o h' (by simp [State.kidsOf_setKids]) ho with h'' | h''
      · simp at h''
        rcases h'' with h'' | h''
        · exact Or.inl h''
        · subst h''; exact Or.inr h'
      · exact Or.inr h''

/-- `cap_revoke_below`'s loop: each node it takes is no pool's own node, so what was below it comes up derived. -/
theorem revokeLoop_parented (m : Machine) (t slot root : Nat) (r : List Nat) :
    ∀ (fuel : Nat) (s : State) (acc outs : List Outcome), Forest s → Parented m s →
      revokeLoop m t slot root r fuel s acc = some outs → ∀ o ∈ outs, o ∈ acc ∨ Parented m o.st := by
  intro fuel
  induction fuel with
  | zero => intro s acc outs _ _ h; simp [revokeLoop] at h
  | succ fuel ih =>
    intro s acc outs hf hp h o ho
    unfold revokeLoop at h
    split at h
    · simp at h; subst h; simp at ho
      rcases ho with ho | ho
      · exact Or.inl ho
      · subst ho; exact Or.inr hp
    rename_i n _ hk
    have hn : n ∈ s.kidsOf root := by rw [hk]; simp
    dsimp only at h
    split at h
    · simp at h
    rename_i c hc
    split at h
    · simp at h
    rename_i hnr
    have hf' := hf.detachErase hn
    have hp' := hp.detachErase hf hn hc (by simpa using hnr)
    repeat' (split at h)
    all_goals first
      | (simp only [Option.some.injEq] at h; subst h; simp at ho
         rcases ho with ho | ho
         · exact Or.inl ho
         · subst ho; exact Or.inr hp')
      | (rcases ih _ _ _ hf' hp' h o ho with h' | h'
         · first
             | exact Or.inl h'
             | (simp at h'
                rcases h' with h' | h'
                · exact Or.inl h'
                · subst h'; exact Or.inr hp')
         · exact Or.inr h')

/-! What the lookups find, by the tables. -/

theorem slotTable_table {t slot : Nat} {o : Obj} (h : slotTable m s t slot = some o) :
    o ∈ s.objs ∧ o.type = capCaptable := by
  unfold slotTable at h
  cases hp : threadProcess m s t with
  | none => simp [hp] at h
  | some p =>
    simp only [hp] at h
    split at h
    · unfold processTable at h
      cases hc : s.cap? (p + m.procTablesOff + nodeSize * (slot / 2 ^ slotTableShift)) with
      | none => simp [hc] at h
      | some c =>
        cases ho : s.obj? c.a with
        | none => simp [hc, ho] at h
        | some o' =>
          simp [hc, ho, guard] at h
          by_cases h1 : c.type = capCaptable <;> by_cases h2 : o'.type = capCaptable <;>
            simp [h1, h2, failure] at h
          subst h
          exact ⟨List.mem_of_find?_eq_some ho, h2⟩
    · simp at h

/-- A node a thread reaches through a slot lies in a table. -/
theorem Held.table {t n : Nat} (h : Held m s t n) :
    ∃ o i, o ∈ s.objs ∧ o.type = capCaptable ∧ i < o.count ∧ n = nodeAt o i := by
  obtain ⟨slot, hs⟩ := h
  unfold slotAt at hs
  cases ht : slotTable m s t slot with
  | none => simp [ht] at hs
  | some o =>
    simp only [ht, Option.bind_eq_bind, Option.bind_some] at hs
    split at hs
    · cases hs; exact ⟨o, _, (slotTable_table ht).1, (slotTable_table ht).2, ‹_›, rfl⟩
    · simp at hs

/-- So what a thread reaches is of the ABI's types. -/
theorem Parented.held {t n : Nat} {c : Cap} (h : Parented m s) (hh : Held m s t n) (hc : s.cap? n = some c) :
    c.type < capInstalled := by
  obtain ⟨o, i, ho, ht, hi, rfl⟩ := hh.table
  exact h.tables o ho ht i hi c hc

/-- A delete of a table's slot: of an empty one nothing, of a node with a parent a detach, of a root its steps. -/
theorem capDelete_at {table : Obj} {r : List Nat} {outs : List Outcome} (h : capDelete s table r = .ok (some outs)) :
    outs = [finish s r] ∨ reg r 1 < table.count ∧ ∃ c, s.cap? (nodeAt table (reg r 1)) = some c ∧
      ((∃ p, s.parent? (nodeAt table (reg r 1)) = some p ∧
          outs = [finish ((s.detachAdopt (nodeAt table (reg r 1))).eraseCap (nodeAt table (reg r 1))) r]) ∨
        (s.parent? (nodeAt table (reg r 1)) = none ∧
          outs = deleteRoot (nodeAt table (reg r 1)) r (s.kidsOf (nodeAt table (reg r 1)))
            (s.setCap (nodeAt table (reg r 1)) c.markGoing) [])) := by
  unfold capDelete at h
  split at h
  · simp at h
  rename_i hlt
  dsimp only at h
  split at h
  · simp at h; exact Or.inl h.symm
  rename_i c hc
  refine Or.inr ⟨by omega, c, hc, ?_⟩
  split at h
  · rename_i p hp; simp at h; exact Or.inl ⟨p, hp, h.symm⟩
  · rename_i hp; simp at h; exact Or.inr ⟨hp, h.symm⟩

/-! Each handler, in every outcome it names. -/

theorem one_parented {o : Outcome} {x : State} {k : Ret} (hx : Parented m x) (ho : o ∈ [(⟨x, k⟩ : Outcome)]) :
    Parented m o.st := by
  rw [List.mem_singleton] at ho; subst ho; exact hx

/-- A copy beside its source, derived from the source's parent; a derive below it, from the source. -/
theorem capCopy_parented (h : Parented m s) (hf : Forest s) {t : Nat} {derive : Bool} {table : Obj} {r : List Nat}
    {outs : List Outcome} (hh : capCopy m s t derive table r = .ok (some outs)) : ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  obtain ⟨source, src, dst, hl, hdst, rfl⟩ := capCopy_ok hh
  obtain ⟨held, hsrc⟩ := slotLookup_held hl
  have hty := h.held held hsrc
  refine one_parented ?_ ho
  cases derive
  · exact h.beside hf hdst hty (fun q cq hq hcq => (h.edges _ _ _ _ hq hcq hsrc).narrow rfl rfl rfl (sub_mask _ _))
  · exact h.store hf hdst hsrc hty (Derives.masked hty _)

theorem capMove_parented (h : Parented m s) (hf : Forest s) {t : Nat} {table : Obj} {r : List Nat}
    {outs : List Outcome} (hh : capMove m s t table r = .ok (some outs)) : ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  obtain ⟨_, _, _, hl, hdst, rfl⟩ := capMove_ok hh
  obtain ⟨held, hsrc⟩ := slotLookup_held hl
  exact one_parented (h.move hf hsrc hdst (h.held held hsrc)) ho

/-- A delete takes a table's slot, which holds no pool's own node. -/
theorem capDelete_parented (h : Parented m s) (hf : Forest s) {table : Obj} (hmem : table ∈ s.objs)
    (htt : table.type = capCaptable) {r : List Nat} {outs : List Outcome}
    (hh : capDelete s table r = .ok (some outs)) : ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  rcases capDelete_at hh with rfl | ⟨hlt, c, hc, ⟨p, hp, rfl⟩ | ⟨hp, rfl⟩⟩
  · exact one_parented h ho
  · have hty := h.tables table hmem htt _ hlt c hc
    exact one_parented (h.detachErase hf ((hf.parent?_eq _ p).mp hp) hc (by unfold_types; omega)) ho
  · rcases deleteRoot_parented _ r _ _ [] o (h.markGoing hc ((hf.parent?_none _).mp hp)) rfl ho with h' | h'
    · simp at h'
    · exact h'

/-- A carve: a frame within the frame, with its rights. -/
theorem opFrame_parented (h : Parented m s) (hf : Forest s) (hd : Derived s) {t op node : Nat} {c : Cap}
    {r : List Nat} {outs : List Outcome} (hc : s.cap? node = some c) (hty : c.type = capFrame)
    (hh : opFrame m s t op node c r = .ok (some outs)) : ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  obtain ⟨dst, -, hoff, hlen, hdst, rfl⟩ := opFrame_ok hh
  have hfit := hd.fits node c hc hty
  have hbase : (BitVec.ofNat 32 c.a + BitVec.ofNat 32 (reg r 1)).toNat = c.a + reg r 1 := by
    simp only [BitVec.toNat_add, BitVec.toNat_ofNat]; omega
  refine one_parented ?_ ho
  refine h.store hf hdst hc (by simp; decide) (.frame (show kernelType capFrame = capFrame by decide) (by rw [hty]; decide)
    (sub_self _) ?_)
  simp only [hbase, within, Bool.and_eq_true, decide_eq_true_eq]; omega

/-- A retype into a frame: a frame of the whole block, with the Untyped's rights. -/
theorem untypedRetype_parented (h : Parented m s) (hf : Forest s) {t u : Nat} {c : Cap} {r : List Nat}
    {outs : List Outcome} (hc : s.cap? u = some c) (hty : c.type = capUntyped)
    (hh : untypedRetype m s t u c r = .ok (some outs)) : ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  obtain ⟨dst, ret, hdst, rfl⟩ := untypedRetype_ok hh
  refine one_parented ?_ ho
  exact h.store hf hdst hc (by simp; decide) (.untyped hty (State.range?_frame s rfl) (State.range?_untyped s hty)
    (Or.inr (Or.inl (show kernelType capFrame = capFrame by decide))) (Or.inl (sub_self _)) (within_self _ _))

/-- A split: two halves, each within the block, where the smallest region is two bytes or more. -/
theorem untypedSplit_parented (hm : 2 ≤ m.regionMin) (h : Parented m s) (hf : Forest s) {t u : Nat} {c : Cap}
    {r : List Nat} {outs : List Outcome} (hc : s.cap? u = some c) (hty : c.type = capUntyped)
    (hh : untypedSplit m s t u c r = .ok (some outs)) : ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  obtain ⟨lo, hi, hlo, hhi, hne, hnapot, rfl⟩ := untypedSplit_ok hh
  obtain ⟨pow, aligned⟩ := napot_split _ _ _ hnapot
  have hmin : m.regionMin ≤ (blockSize (BitVec.ofNat 32 c.a) / 2).toNat := by
    simp only [napot, Bool.and_eq_true, decide_eq_true_eq] at hnapot; exact hnapot.1.1
  have h2 : (2#32 : BitVec 32).toNat = 2 := rfl
  have hbig : 2#32 ≤ blockSize (BitVec.ofNat 32 c.a) / 2 := by rw [BitVec.le_def, h2]; omega
  obtain ⟨sl, -, sh, -⟩ := split_halves (BitVec.ofNat 32 c.a) 0 pow hbig aligned
  -- Each half lies within the block: a half is no empty block at zero, being two bytes or more.
  have half : ∀ (st : State) (lower : Bool),
      Derives m st ⟨capUntyped, c.rights, 0, (blockWord (if lower then blockBase (BitVec.ofNat 32 c.a)
        else blockBase (BitVec.ofNat 32 c.a) + blockSize (BitVec.ofNat 32 c.a) / 2)
        (blockSize (BitVec.ofNat 32 c.a) / 2)).toNat, 0⟩ c := by
    intro st lower
    have hw := half_within (BitVec.ofNat 32 c.a) lower pow aligned
    have hz : (blockSize (blockWord (if lower then blockBase (BitVec.ofNat 32 c.a)
        else blockBase (BitVec.ofNat 32 c.a) + blockSize (BitVec.ofNat 32 c.a) / 2)
        (blockSize (BitVec.ofNat 32 c.a) / 2))).toNat ≠ 0 := by
      cases lower
      · simp only [Bool.false_eq_true, ↓reduceIte]; rw [sh]; omega
      · simp only [↓reduceIte]; rw [sl]; omega
    refine .untyped hty (State.range?_untyped st rfl) (State.range?_untyped st hty) (Or.inl rfl) (Or.inl (sub_self _)) ?_
    simp only [BitVec.ofNat_toNat, BitVec.setWidth_eq]
    rcases hw with ⟨h1, -⟩ | ⟨h1, h2⟩
    · exact absurd h1 hz
    · simp only [within, Bool.and_eq_true, decide_eq_true_eq]; omega
  have hul : u ≠ lo := by intro e; subst e; simp [hc] at hlo
  have h1 := h.store hf hlo hc (by simp; decide) (half s true)
  refine one_parented ?_ ho
  refine h1.store (hf.store _ hlo (by simp [hc])) ?_ ?_ (by simp; decide) (half _ false)
  · rw [cap?_store]; simp [Ne.symm hne, hhi]
  · rw [cap?_store]; simp [hul, hc]

/-- Every modelled step keeps every node derived from its parent, and tables of the ABI's types:
every outcome it names, a call stopped part way among them, where the smallest region is two bytes or more. -/
theorem step_parented (m : Machine) (s : State) (e : Event) (outs : List Outcome) (hm : 2 ≤ m.regionMin)
    (hf : Forest s) (hd : Derived s) (h : Parented m s) (hs : step m s e = some outs) :
    ∀ o ∈ outs, Parented m o.st := by
  intro o ho
  rcases step_cases hs with ⟨_, rfl⟩ | ⟨t, op, r, slot, node, c, rfl, -, hc, hh⟩
  · exact one_parented h ho
  rcases hh with ⟨_, -, hmem, htt, hh⟩ | ⟨hty, hh⟩ | ⟨hty, hh⟩
  · rcases opCaptable_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
    · exact capCopy_parented h hf hh o ho
    · exact capCopy_parented h hf hh o ho
    · exact capMove_parented h hf hh o ho
    · exact capDelete_parented h hf hmem htt hh o ho
    · obtain ⟨_, _, hh⟩ := capRevoke_ok hh
      rcases revokeLoop_parented m t _ _ r _ s [] _ hf h hh o ho with h' | h'
      · simp at h'
      · exact h'
  · exact opFrame_parented h hf hd hc hty hh o ho
  · rcases opUntyped_cases hh with ⟨-, hh⟩ | ⟨-, hh⟩
    · exact untypedRetype_parented h hf hc hty hh o ho
    · exact untypedSplit_parented hm h hf hc hty hh o ho

end

/--
The four invariants `model-check` states hold together from step to step:
from a state that holds them, every outcome a step names holds them too,
where the smallest region is two bytes or more.
-/
theorem step_checked (m : Machine) (s : State) (e : Event) (outs : List Outcome) (hm : 2 ≤ m.regionMin)
    (hn : Nested s) (hp : Parented m s) (hs : step m s e = some outs) : ∀ o ∈ outs, Nested o.st ∧ Parented m o.st :=
  fun o ho => ⟨step_nested m s e outs hn hs o ho, step_parented m s e outs hm hn.forest hn.derived hp hs o ho⟩

end Rvuos
