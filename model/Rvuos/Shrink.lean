/-
Delete and revoke only take away.
Every capability a delete or a revoke leaves was there before it, with the same words,
but for an Untyped a delete marked going, which then makes nothing,
in every outcome the model names, a call stopped part way among them.
So neither can make authority, however the tree around the slot lies;
`model-check` holds the kernel to the model on every step the corpus makes.
-/
import Rvuos.Calls

namespace Rvuos

/-- A node's words as a delete or a revoke leaves them: as they were, or an Untyped's marked going. -/
def Kept (c₀ c : Cap) : Prop := c = c₀ ∨ c = c₀.markGoing

theorem Cap.markGoing_markGoing (c : Cap) : c.markGoing.markGoing = c.markGoing := by
  unfold Cap.markGoing
  split <;> simp_all

theorem Kept.trans {c₁ c₂ c₃ : Cap} (h₁ : Kept c₁ c₂) (h₂ : Kept c₂ c₃) : Kept c₁ c₃ := by
  rcases h₁ with rfl | rfl <;> rcases h₂ with rfl | rfl <;> simp [Kept, Cap.markGoing_markGoing]

/-- Every node `s'` fills, `s` filled with the same words, or an Untyped's that `s'` marks going. -/
def Shrinks (s s' : State) : Prop := ∀ n c, s'.cap? n = some c → ∃ c₀, s.cap? n = some c₀ ∧ Kept c₀ c

theorem Shrinks.of_cap? {s s' : State} (h : ∀ n c, s'.cap? n = some c → s.cap? n = some c) : Shrinks s s' :=
  fun n c hc => ⟨c, h n c hc, Or.inl rfl⟩

theorem Shrinks.refl (s : State) : Shrinks s s := Shrinks.of_cap? fun _ _ h => h

theorem Shrinks.trans {s₁ s₂ s₃ : State} (h₁ : Shrinks s₁ s₂) (h₂ : Shrinks s₂ s₃) : Shrinks s₁ s₃ := by
  intro n c h
  obtain ⟨c₂, h₂', k₂⟩ := h₂ n c h
  obtain ⟨c₁, h₁', k₁⟩ := h₁ n c₂ h₂'
  exact ⟨c₁, h₁', k₁.trans k₂⟩

theorem State.eraseCap_shrinks (s : State) (n : Nat) : Shrinks s (s.eraseCap n) := by
  refine Shrinks.of_cap? fun k c h => ?_
  rw [State.cap?_eraseCap] at h
  split at h
  · simp at h
  · exact h

theorem State.setKids_shrinks (s : State) (n : Nat) (l : List Nat) : Shrinks s (s.setKids n l) :=
  Shrinks.of_cap? fun k c h => by rwa [State.cap?_setKids] at h

/-- A delete of a root marks it going first. -/
theorem State.going_shrinks {s : State} {n : Nat} {c : Cap} (hc : s.cap? n = some c) :
    Shrinks s (s.setCap n c.markGoing) := by
  intro k d h
  rw [State.cap?_setCap] at h
  split at h
  · rename_i e; subst e; simp at h; exact ⟨c, hc, Or.inr h.symm⟩
  · exact ⟨d, h, Or.inl rfl⟩

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

theorem capDelete_shrinks {s : State} {table : Obj} {r : List Nat} {outs : List Outcome}
    (h : capDelete s table r = .ok (some outs)) : ∀ o ∈ outs, Shrinks s o.st := by
  intro o ho
  rcases capDelete_ok h with rfl | ⟨n, ⟨_, _, rfl⟩ | ⟨_, hc, _, rfl⟩⟩
  · simp at ho; subst ho; exact Shrinks.refl s
  · simp at ho; subst ho; exact revoke_step_shrinks s n
  · rcases deleteRoot_shrinks _ _ _ _ [] o ho with h' | h'
    · simp at h'
    · exact (State.going_shrinks hc).trans h'

theorem capRevoke_shrinks {m : Machine} {s : State} {t slot : Nat} {table : Obj} {r : List Nat}
    {outs : List Outcome} (h : capRevoke m s t slot table r = .ok (some outs)) : ∀ o ∈ outs, Shrinks s o.st := by
  intro o ho
  obtain ⟨_, _, h⟩ := capRevoke_ok h
  rcases revokeLoop_shrinks m t _ _ r _ s [] _ h o ho with h' | h'
  · simp at h'
  · exact h'

/-- A delete or a revoke, in every outcome the model names: nothing that was not there before, with its words or marked going. -/
theorem delete_revoke_shrink (m : Machine) (s : State) (t op : Nat) (r : List Nat) (outs : List Outcome)
    (hop : op = opCapDelete ∨ op = opCapRevoke) (h : step m s (.call t op r) = some outs) :
    ∀ o ∈ outs, Shrinks s o.st := by
  rcases step_cases h with ⟨_, rfl⟩ | ⟨_, _, _, _, _, _, he, -, -, hh⟩
  · intro o ho; simp at ho; subst ho; exact Shrinks.refl s
  cases he
  -- Only a table takes a delete or a revoke.
  have ne : ∀ op', op' ≠ opCapDelete → op' ≠ opCapRevoke → op ≠ op' := by
    rintro op' h₁ h₂ rfl; rcases hop with h | h <;> contradiction
  rcases hh with ⟨_, -, hh⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
  · rcases opCaptable_cases hh with ⟨e, -⟩ | ⟨e, -⟩ | ⟨e, -⟩ | ⟨-, hh⟩ | ⟨-, hh⟩
    any_goals exact absurd e (ne _ (by decide) (by decide))
    · exact capDelete_shrinks hh
    · exact capRevoke_shrinks hh
  · obtain ⟨_, e, -⟩ := opFrame_ok hh
    exact absurd e (ne _ (by decide) (by decide))
  · rcases opUntyped_cases hh with ⟨e, -⟩ | ⟨e, -⟩ <;> exact absurd e (ne _ (by decide) (by decide))

end Rvuos
