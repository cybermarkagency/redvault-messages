import { Avatar } from "./Avatar.js";

export interface GroupGlyphProps {
  /** The group's name: its initials go on the tile. */
  label?: string;
  /** Members' handles, used when the group has no name. */
  memberSeeds: string[];
  size?: number;
}

/** Groups are a rounded square with the group's initials, like the design. */
export function GroupGlyph({ label, memberSeeds, size = 40 }: GroupGlyphProps) {
  return <Avatar seed={label?.trim() || memberSeeds.slice(0, 2).join(" ") || "Group"} size={size} shape="square" />;
}
