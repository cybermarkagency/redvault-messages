import { fireEvent, screen } from "@testing-library/react";

/** Ticks the three sign-up boxes, as a user would before Create Account. */
export async function tickSignUpBoxes(): Promise<void> {
  fireEvent.click(screen.getByRole("checkbox", { name: /terms of service/i }));
  fireEvent.click(screen.getByRole("checkbox", { name: /privacy policy/i }));
  fireEvent.click(screen.getByRole("checkbox", { name: /18 or older/i }));
}
