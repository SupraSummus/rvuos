/-
What each call the model names leaves when it goes through, said once for the proofs:
`Shrink.lean`, `Flows.lean` and `Forest.lean` reason from these, not from the handlers' code.
An operation the model comes to name adds its lemma here and its case to each of them.
-/
import Rvuos.Step

namespace Rvuos

/-! What each change of a state does to its words and its lists. -/

theorem assoc_cons {α : Type} (n k : Nat) (v : α) (l : List (Nat × α)) :
    assoc ((n, v) :: l) k = if n = k then some v else assoc l k := rfl

theorem assoc_unkey {α : Type} (l : List (Nat × α)) (n k : Nat) :
    assoc (unkey l n) k = if k = n then none else assoc l k := by
  induction l with
  | nil => simp [assoc, unkey]
  | cons x xs ih =>
    obtain ⟨x1, x2⟩ := x
    simp only [unkey, List.filter_cons] at ih ⊢
    by_cases hx : x1 = n
    · subst hx
      simp only [ne_eq, not_true_eq_false, decide_false, Bool.false_eq_true, ↓reduceIte, ih]
      by_cases hk : k = x1
      · simp [hk]
      · simp [assoc, hk, Ne.symm hk]
    · simp only [ne_eq, hx, not_false_eq_true, decide_true, ↓reduceIte, assoc, ih]
      by_cases hk : k = n
      · subst hk; simp [hx]
      · simp [hk]

namespace State

theorem cap?_setCap (s : State) (n k : Nat) (c : Cap) :
    (s.setCap n c).cap? k = if k = n then some c else s.cap? k := by
  simp only [cap?, setCap, assoc_cons, assoc_unkey]
  by_cases h : k = n
  · subst h; simp
  · simp [h, Ne.symm h]

theorem cap?_eraseCap (s : State) (n k : Nat) : (s.eraseCap n).cap? k = if k = n then none else s.cap? k := by
  simp only [cap?, eraseCap, assoc_unkey]

theorem cap?_setKids (s : State) (n k : Nat) (l : List Nat) : (s.setKids n l).cap? k = s.cap? k := by
  unfold setKids; rfl

theorem cap?_attach (s : State) (p : Option Nat) (n k : Nat) : (s.attach p n).cap? k = s.cap? k := by
  unfold attach; split <;> simp [cap?_setKids]

theorem cap?_attachBeside (s : State) (b n k : Nat) : (s.attachBeside b n).cap? k = s.cap? k := by
  unfold attachBeside; split <;> simp [cap?_setKids]

theorem cap?_move (s : State) (src dst k : Nat) (c : Cap) (h : s.cap? src = some c) :
    (s.move src dst).cap? k = if k = dst then some c else if k = src then none else s.cap? k := by
  unfold move
  simp only [h]
  split <;> simp [cap?_setKids, cap?_setCap, cap?_eraseCap]

theorem kidsOf_setKids (s : State) (n k : Nat) (l : List Nat) :
    (s.setKids n l).kidsOf k = if k = n then l else s.kidsOf k := by
  unfold kidsOf setKids
  by_cases hl : l.isEmpty
  · simp only [hl, ↓reduceIte, assoc_unkey]
    by_cases hk : k = n
    · simp [hk, List.isEmpty_iff.mp hl]
    · simp [hk]
  · simp only [hl, Bool.false_eq_true, ↓reduceIte, assoc_cons, assoc_unkey]
    by_cases hk : k = n
    · simp [hk]
    · simp [hk, Ne.symm hk]

theorem kidsOf_setCap (s : State) (n k : Nat) (c : Cap) : (s.setCap n c).kidsOf k = s.kidsOf k := rfl

theorem kidsOf_eraseCap (s : State) (n k : Nat) : (s.eraseCap n).kidsOf k = s.kidsOf k := rfl

end State

theorem cap?_store (s : State) (n k : Nat) (c : Cap) (p : Option Nat) :
    (store s n c p).cap? k = if k = n then some c else s.cap? k := by
  simp [store, State.cap?_attach, State.cap?_setCap]

/-! What the lookups find. -/

/-- A node the thread at `t` reaches through a slot of its tables. -/
def Held (m : Machine) (s : State) (t n : Nat) : Prop := ∃ slot, slotAt m s t slot = some n

def Event.thread : Event → Nat
  | .call t _ _ => t
  | .fault t => t

section
variable {m : Machine} {s : State}

theorem capLookup_ok {o : Obj} {idx : Nat} {c : Cap} (h : capLookup s o idx = .ok c) :
    idx < o.count ∧ s.cap? (nodeAt o idx) = some c := by
  unfold capLookup at h
  repeat' split at h
  all_goals simp_all

theorem slotLookup_held {t slot n : Nat} {c : Cap} (h : slotLookup m s t slot = .ok (n, c)) :
    Held m s t n ∧ s.cap? n = some c := by
  unfold slotLookup at h
  split at h
  · rename_i o ho
    cases hl : capLookup s o (slotIndex slot) with
    | error k => simp [hl] at h; contradiction
    | ok c' =>
      have h' : (nodeAt o (slotIndex slot), c') = (n, c) := by simpa [Functor.map, Except.map, hl] using h
      obtain ⟨rfl, rfl⟩ := Prod.mk.inj h'
      obtain ⟨hlt, hc⟩ := capLookup_ok hl
      exact ⟨⟨slot, by simp [slotAt, ho, hlt]⟩, hc⟩
  · simp at h

theorem capSlotFree_empty {o : Obj} {idx n : Nat} (h : capSlotFree s o idx = .ok n) : s.cap? n = none := by
  unfold capSlotFree at h
  repeat' split at h
  all_goals simp_all

theorem slotVacant_empty {t slot n : Nat} (h : slotVacant m s t slot = .ok n) : s.cap? n = none := by
  unfold slotVacant at h
  split at h
  · exact capSlotFree_empty h
  · simp at h

/-! Each handler, when it goes through. -/

/-- A copy fills an empty slot beside its source, a derive below it, with the rights the mask leaves. -/
theorem capCopy_ok {t : Nat} {derive : Bool} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capCopy m s t derive table r = .ok (some outs)) :
    ∃ source src dst, slotLookup m s t (reg r 2) = .ok (source, src) ∧ s.cap? dst = none ∧
      let c := { src with rights := src.rights &&& (reg r 3 % 256) }
      outs = [finish (if derive then store s dst c (some source) else (s.setCap dst c).attachBeside source dst) r] := by
  unfold capCopy at h
  cases hl : slotLookup m s t (reg r 2) with
  | error k => simp [hl, bind, Except.bind] at h
  | ok p =>
    obtain ⟨source, src⟩ := p
    simp only [hl, bind, Except.bind] at h
    cases hf : capSlotFree s table (reg r 1) with
    | error k =>
      simp only [hf] at h
      repeat' split at h
      all_goals simp_all [throw, throwThe, MonadExceptOf.throw]
    | ok dst =>
      refine ⟨source, src, dst, rfl, capSlotFree_empty hf, ?_⟩
      simp only [hf] at h
      repeat' split at h
      all_goals simp_all [throw, throwThe, MonadExceptOf.throw, pure, Except.pure]

/-- A move takes the source to an empty slot. -/
theorem capMove_ok {t : Nat} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capMove m s t table r = .ok (some outs)) :
    ∃ source src dst, slotLookup m s t (reg r 2) = .ok (source, src) ∧ s.cap? dst = none ∧
      outs = [finish (s.move source dst) r] := by
  unfold capMove at h
  cases hl : slotLookup m s t (reg r 2) with
  | error k => simp [hl, bind, Except.bind] at h
  | ok p =>
    obtain ⟨source, src⟩ := p
    cases hf : capSlotFree s table (reg r 1) with
    | error k => simp [hl, hf, bind, Except.bind] at h
    | ok dst =>
      simp [hl, hf, bind, Except.bind, pure, Except.pure] at h
      exact ⟨source, src, dst, rfl, capSlotFree_empty hf, h.symm⟩

/-- A delete of an empty slot changes nothing; of a node with a parent it detaches it; of a root it goes a child a step. -/
theorem capDelete_ok {table : Obj} {r : List Nat} {outs : List Outcome} (h : capDelete s table r = .ok (some outs)) :
    outs = [finish s r] ∨ ∃ n,
      (∃ p, s.parent? n = some p ∧ outs = [finish ((s.detachAdopt n).eraseCap n) r]) ∨
      (s.parent? n = none ∧ outs = deleteRoot n r (s.kidsOf n) s []) := by
  unfold capDelete at h
  dsimp only at h
  repeat' split at h
  all_goals simp at h
  · exact Or.inl h.symm
  · rename_i p hp; exact Or.inr ⟨_, Or.inl ⟨p, hp, h.symm⟩⟩
  · rename_i hp; exact Or.inr ⟨_, Or.inr ⟨hp, h.symm⟩⟩

theorem capRevoke_ok {t slot : Nat} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capRevoke m s t slot table r = .ok (some outs)) : ∃ root fuel, revokeLoop m t slot root r fuel s [] = some outs := by
  unfold capRevoke at h
  dsimp only at h
  repeat' split at h
  all_goals simp at h
  exact ⟨_, _, h⟩

/-- A carve fills an empty slot below the frame with a range within it. -/
theorem opFrame_ok {t op node : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (h : opFrame m s t op node c r = .ok (some outs)) :
    ∃ dst, op = opFrameCarve ∧ reg r 1 < c.b ∧ reg r 2 ≤ c.b - reg r 1 ∧ s.cap? dst = none ∧
      outs = [finish (store s dst ⟨capFrame, c.rights, 0, (BitVec.ofNat 32 c.a + BitVec.ofNat 32 (reg r 1)).toNat, reg r 2⟩
        (some node)) r] := by
  unfold opFrame at h
  split at h
  · simp at h
  · rename_i hop
    simp only [bind, Except.bind] at h
    split at h
    · simp [throw, throwThe, MonadExceptOf.throw] at h
    · rename_i hchk
      simp only [Bool.or_eq_true, decide_eq_true_eq, not_or] at hchk
      cases hv : slotVacant m s t (reg r 3) with
      | error k => simp [hv] at h
      | ok dst =>
        simp [hv, pure, Except.pure] at h
        refine ⟨dst, by simpa using hop, by omega, by omega, slotVacant_empty hv, ?_⟩
        rw [← h]; simp

/-- A retype into a frame fills an empty slot below the Untyped with a frame of its whole block. -/
theorem untypedRetype_ok {t u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (h : untypedRetype m s t u c r = .ok (some outs)) :
    ∃ dst ret, s.cap? dst = none ∧ outs = [⟨store s dst
      ⟨capFrame, c.rights, 0, (blockBase (BitVec.ofNat 32 c.a)).toNat, (blockSize (BitVec.ofNat 32 c.a)).toNat⟩ (some u),
      ret⟩] := by
  unfold untypedRetype at h
  simp only [bind, Except.bind] at h
  cases hv : slotVacant m s t (reg r 2) with
  | error k =>
    simp only [hv] at h
    repeat' split at h
    all_goals simp_all [throw, throwThe, MonadExceptOf.throw]
  | ok dst =>
    simp only [hv] at h
    repeat' split at h
    all_goals try (simp [throw, throwThe, MonadExceptOf.throw] at *; done)
    all_goals simp only [pure, Except.pure, Except.ok.injEq, Option.some.injEq, reduceCtorEq] at h
    exact ⟨dst, _, slotVacant_empty hv, h.symm⟩

/-- A split fills two empty slots below the Untyped, the lower half first, where `napot` lets it. -/
theorem untypedSplit_ok {t u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (h : untypedSplit m s t u c r = .ok (some outs)) :
    let base := blockBase (BitVec.ofNat 32 c.a)
    let half := blockSize (BitVec.ofNat 32 c.a) / 2
    ∃ lo hi, s.cap? lo = none ∧ s.cap? hi = none ∧ lo ≠ hi ∧ napot m.regionMin base half = true ∧
      outs = [finish (store (store s lo ⟨capUntyped, c.rights, 0, (blockWord base half).toNat, 0⟩ (some u))
        hi ⟨capUntyped, c.rights, 0, (blockWord (base + half) half).toNat, 0⟩ (some u)) r] := by
  unfold untypedSplit at h
  simp only [bind, Except.bind] at h
  cases hlo : slotVacant m s t (reg r 1) with
  | error k =>
    simp only [hlo] at h
    repeat' split at h
    all_goals simp_all [throw, throwThe, MonadExceptOf.throw]
  | ok lo =>
  cases hhi : slotVacant m s t (reg r 2) with
  | error k =>
    simp only [hlo, hhi] at h
    repeat' split at h
    all_goals simp_all [throw, throwThe, MonadExceptOf.throw]
  | ok hi =>
    simp only [hlo, hhi] at h
    repeat' split at h
    all_goals try (simp [throw, throwThe, MonadExceptOf.throw] at *; done)
    rename_i hnapot hne _
    simp [pure, Except.pure] at h hnapot hne
    exact ⟨lo, hi, slotVacant_empty hlo, slotVacant_empty hhi, hne, hnapot, h.symm⟩

/-! A step. -/

/-- A step leaves the state as it was, or goes through a handler on a node the caller holds. -/
theorem step_cases {e : Event} {outs : List Outcome} (h : step m s e = some outs) :
    (∃ k, outs = [⟨s, k⟩]) ∨
    ∃ t op r slot node c, e = .call t op r ∧ Held m s t node ∧ s.cap? node = some c ∧
      ((∃ named, c.type = capCaptable ∧ opCaptable m s t slot op named r = .ok (some outs)) ∨
       (c.type = capFrame ∧ opFrame m s t op node c r = .ok (some outs)) ∨
       (c.type = capUntyped ∧ opUntyped m s t op node c r = .ok (some outs))) := by
  cases e with
  | fault t => simp [step] at h; exact Or.inl ⟨_, h.symm⟩
  | call t op r =>
    simp only [step] at h
    split at h
    · simp at h; exact Or.inl ⟨_, h.symm⟩
    rename_i table htable
    split at h
    · simp at h; exact Or.inl ⟨_, h.symm⟩
    rename_i c hc
    obtain ⟨hlt, hnode⟩ := capLookup_ok hc
    have held : Held m s t (nodeAt table (slotIndex (reg r 0))) := ⟨reg r 0, by simp [slotAt, htable, hlt]⟩
    split at h
    · split at h <;> (simp at h; exact Or.inl ⟨_, h.symm⟩)
    split at h
    · simp at h; exact Or.inl ⟨_, h.symm⟩
    · simp at h
    split at h
    · simp at h; exact Or.inl ⟨_, h.symm⟩
    rename_i hans
    subst h
    refine Or.inr ⟨t, op, r, reg r 0, _, c, rfl, held, hnode, ?_⟩
    split at hans
    · rename_i hty
      split at hans
      · exact Or.inl ⟨_, by simpa using hty, hans⟩
      · simp at hans
    · split at hans
      · rename_i hty; exact Or.inr (Or.inl ⟨by simpa using hty, hans⟩)
      · split at hans
        · rename_i hty; exact Or.inr (Or.inr ⟨by simpa using hty, hans⟩)
        · simp at hans

theorem opCaptable_cases {t slot op : Nat} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : opCaptable m s t slot op table r = .ok (some outs)) :
    (op = opCapCopy ∧ capCopy m s t false table r = .ok (some outs)) ∨
    (op = opCapDerive ∧ capCopy m s t true table r = .ok (some outs)) ∨
    (op = opCapMove ∧ capMove m s t table r = .ok (some outs)) ∨
    (op = opCapDelete ∧ capDelete s table r = .ok (some outs)) ∨
    (op = opCapRevoke ∧ capRevoke m s t slot table r = .ok (some outs)) := by
  unfold opCaptable at h
  repeat' split at h
  all_goals simp_all

theorem opUntyped_cases {t op u : Nat} {c : Cap} {r : List Nat} {outs : List Outcome}
    (h : opUntyped m s t op u c r = .ok (some outs)) :
    (op = opUntypedRetype ∧ untypedRetype m s t u c r = .ok (some outs)) ∨
    (op = opUntypedSplit ∧ untypedSplit m s t u c r = .ok (some outs)) := by
  unfold opUntyped at h
  repeat' split at h
  all_goals simp_all

end

end Rvuos
