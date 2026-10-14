/-
Delete and revoke only take away.
Every capability a delete or a revoke leaves was there before it, with the same words,
in every outcome the model names, a call stopped part way among them.
So neither can make authority, however the tree around the slot lies;
`model-check` holds the kernel to the model on every step the corpus makes.
-/
import Rvuos.Step

namespace Rvuos

/-- Every node `s'` fills, `s` filled with the same words. -/
def Shrinks (s s' : State) : Prop := ∀ n c, s'.cap? n = some c → s.cap? n = some c

theorem Shrinks.refl (s : State) : Shrinks s s := fun _ _ h => h

theorem Shrinks.trans {s₁ s₂ s₃ : State} (h₁ : Shrinks s₁ s₂) (h₂ : Shrinks s₂ s₃) : Shrinks s₁ s₃ :=
  fun n c h => h₁ n c (h₂ n c h)

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

theorem State.eraseCap_shrinks (s : State) (n : Nat) : Shrinks s (s.eraseCap n) := by
  intro k c h
  simp only [State.cap?, State.eraseCap, assoc_unkey] at h
  split at h
  · simp at h
  · exact h

theorem State.setKids_caps (s : State) (n : Nat) (l : List Nat) : (s.setKids n l).caps = s.caps := by
  unfold State.setKids; rfl

theorem State.setKids_shrinks (s : State) (n : Nat) (l : List Nat) : Shrinks s (s.setKids n l) := by
  intro k c h
  simpa [State.cap?, State.setKids_caps] using h

theorem State.detachAdopt_shrinks (s : State) (n : Nat) : Shrinks s (s.detachAdopt n) := by
  unfold State.detachAdopt
  split
  · exact Shrinks.refl s
  · exact Shrinks.trans (State.setKids_shrinks _ _ _) (State.setKids_shrinks _ _ _)

theorem deleteRoot_shrinks (n : Nat) (r : List Nat) (ks : List Nat) :
    ∀ (s : State) (acc : List Outcome) (o : Outcome),
      o ∈ deleteRoot n r ks s acc → o ∈ acc ∨ Shrinks s o.st := by
  induction ks with
  | nil =>
    intro s acc o h
    simp [deleteRoot] at h
    rcases h with h | h
    · exact Or.inl h
    · subst h; exact Or.inr (State.eraseCap_shrinks s n)
  | cons x xs ih =>
    intro s acc o h
    cases xs with
    | nil =>
      simp [deleteRoot] at h
      rcases h with h | h
      · exact Or.inl h
      · subst h
        exact Or.inr (Shrinks.trans (State.setKids_shrinks s n []) (State.eraseCap_shrinks _ n))
    | cons k ks =>
      simp only [deleteRoot] at h
      rcases ih _ _ o h with h' | h'
      · simp at h'
        rcases h' with h' | h'
        · exact Or.inl h'
        · subst h'; exact Or.inr (State.setKids_shrinks s n _)
      · exact Or.inr (Shrinks.trans (State.setKids_shrinks s n _) h')

/-- A step of the revoke clears the root's first child, its children taking its place. -/
theorem revoke_step_shrinks (s : State) (n : Nat) : Shrinks s ((s.detachAdopt n).eraseCap n) :=
  Shrinks.trans (State.detachAdopt_shrinks s n) (State.eraseCap_shrinks _ n)

theorem revokeLoop_shrinks (m : Machine) (t slot root : Nat) (r : List Nat) :
    ∀ (fuel : Nat) (s : State) (acc outs : List Outcome),
      revokeLoop m t slot root r fuel s acc = some outs →
      ∀ o ∈ outs, o ∈ acc ∨ Shrinks s o.st := by
  intro fuel
  induction fuel with
  | zero => intro s acc outs h; simp [revokeLoop] at h
  | succ fuel ih =>
    intro s acc outs h o ho
    unfold revokeLoop at h
    dsimp only at h
    repeat' (split at h)
    all_goals first
      | (simp at h; done)
      | (simp only [Option.some.injEq] at h; subst h; simp at ho
         rcases ho with ho | ho
         · exact Or.inl ho
         · subst ho
           first | exact Or.inr (Shrinks.refl _) | exact Or.inr (revoke_step_shrinks _ _))
      | (rcases ih _ _ _ h o ho with h' | h'
         · first
             | exact Or.inl h'
             | (simp at h'
                rcases h' with h' | h'
                · exact Or.inl h'
                · subst h'; exact Or.inr (revoke_step_shrinks _ _))
         · exact Or.inr (Shrinks.trans (revoke_step_shrinks _ _) h'))

/-- What dispatch let through has the type its operation's shape names. -/
theorem shapeCheck_type {op : Nat} {c : Cap} {r : List Nat} (h : shapeCheck op c r = .ok (some ())) :
    (shapeOf op).type = c.type := by
  unfold shapeCheck at h
  by_cases h1 : ((shapeOf op).type == capNone || (shapeOf op).type != c.type) = true
  · simp [h1] at h
  · simp at h1
    exact h1.2

/-- A delete or a revoke, in every outcome the model names: nothing that was not there before, with its words. -/
theorem delete_revoke_shrink (m : Machine) (s : State) (t op : Nat) (r : List Nat) (outs : List Outcome)
    (hop : op = opCapDelete ∨ op = opCapRevoke) (h : step m s (.call t op r) = some outs) :
    ∀ o ∈ outs, Shrinks s o.st := by
  intro o ho
  have hinfo : (op == opCapInfo) = false := by rcases hop with h | h <;> subst h <;> rfl
  simp only [step, hinfo, Bool.false_eq_true, ↓reduceIte] at h
  split at h
  · simp at h; subst h; simp at ho; subst ho; exact Shrinks.refl s
  split at h
  · simp at h; subst h; simp at ho; subst ho; exact Shrinks.refl s
  rename_i table _ _ c _
  split at h
  · simp at h; subst h; simp at ho; subst ho; exact Shrinks.refl s
  · simp at h
  rename_i hshape
  have hty := shapeCheck_type hshape
  have hcap : c.type = capCaptable := by
    rcases hop with h | h <;> subst h <;> simp [shapeOf] at hty <;> exact hty.symm
  rw [ite_eq_left_of_eq_true _ _ (by simp [hcap])] at h
  split at h
  · simp at h; subst h; simp at ho; subst ho; exact Shrinks.refl s
  rename_i outs' hh
  subst h
  split at hh
  · rename_i named _
    rcases hop with hop | hop <;> subst hop
    · simp [opCaptable] at hh
      unfold capDelete at hh
      dsimp only at hh
      repeat' (split at hh)
      all_goals first
        | (simp at hh; done)
        | (rename_i hne; exact absurd hne (by decide))
        | (simp only [Except.ok.injEq, Option.some.injEq] at hh; subst hh
           first
             | (simp at ho; subst ho
                first | exact Shrinks.refl s | exact revoke_step_shrinks s _)
             | (rcases deleteRoot_shrinks _ _ _ s [] o ho with h' | h'
                · simp at h'
                · exact h'))
    · simp [opCaptable] at hh
      unfold capRevoke at hh
      dsimp only at hh
      repeat' (split at hh)
      all_goals first
        | (simp at hh; done)
        | (rename_i hne; exact absurd hne (by decide))
        | (simp only [Except.ok.injEq] at hh
           rcases revokeLoop_shrinks m t _ _ r _ s [] _ hh o ho with h' | h'
           · simp at h'
           · exact h')
  · simp at hh

end Rvuos
