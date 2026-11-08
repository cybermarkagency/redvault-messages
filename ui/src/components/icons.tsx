// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { SVGProps } from "react";

function Svg(props: SVGProps<SVGSVGElement>) {
  return (
    <svg
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.4}
      strokeLinecap="round"
      strokeLinejoin="round"
      width="1em"
      height="1em"
      aria-hidden="true"
      {...props}
    />
  );
}

export function LockIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <rect x={3.5} y={7} width={9} height={6.5} rx={1.5} />
      <path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2" />
    </Svg>
  );
}

export function HourglassIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M4 2.5h8M4 13.5h8" />
      <path d="M4.5 2.5c0 3 2.5 3.5 3.5 5.5-1 2-3.5 2.5-3.5 5.5M11.5 2.5c0 3-2.5 3.5-3.5 5.5 1 2 3.5 2.5 3.5 5.5" />
    </Svg>
  );
}

export function BackIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M10 3.5 5.5 8l4.5 4.5" />
    </Svg>
  );
}

export function PlusIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M8 3.5v9M3.5 8h9" />
    </Svg>
  );
}

export function CheckIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M3.5 8.5 6.5 11.5 12.5 4.5" />
    </Svg>
  );
}

export function ScanIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M2.5 5.5v-2a1 1 0 0 1 1-1h2M13.5 5.5v-2a1 1 0 0 0-1-1h-2M2.5 10.5v2a1 1 0 0 0 1 1h2M13.5 10.5v2a1 1 0 0 1-1 1h-2M2.5 8h11" />
    </Svg>
  );
}

export function TrashIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M3.5 4.5h9M6.5 4.5V3a1 1 0 0 1 1-1h1a1 1 0 0 1 1 1v1.5M4.5 4.5l.6 8.4a1 1 0 0 0 1 .9h3.8a1 1 0 0 0 1-.9l.6-8.4" />
      <path d="M6.7 7v4M9.3 7v4" />
    </Svg>
  );
}

export function PaperclipIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M13 7.5 8 12.5a3.2 3.2 0 0 1-4.5-4.5l5.3-5.3a2.1 2.1 0 0 1 3 3L6.5 11a1 1 0 0 1-1.5-1.5L10 4.5" />
    </Svg>
  );
}

export function FileIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M4 1.5h5l3.5 3.5v9.5H4z" />
      <path d="M9 1.5V5h3.5" />
    </Svg>
  );
}

export function PhoneIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M5.2 2.2 3.6 2.5a1.3 1.3 0 0 0-1.1 1.4c.4 5 4.6 9.2 9.6 9.6a1.3 1.3 0 0 0 1.4-1.1l.3-1.6-2.7-1.2-1.3 1.3a7 7 0 0 1-3.7-3.7l1.3-1.3z" />
    </Svg>
  );
}

export function VideoIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <rect x={1.5} y={4} width={9} height={8} rx={1.5} />
      <path d="m10.5 7 4-2.2v6.4l-4-2.2" />
    </Svg>
  );
}

export function VideoOffIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <rect x={1.5} y={4} width={9} height={8} rx={1.5} />
      <path d="m10.5 7 4-2.2v6.4l-4-2.2" />
      <path d="M1.5 1.5l13 13" />
    </Svg>
  );
}

export function MicIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <rect x={5.75} y={1.5} width={4.5} height={8} rx={2.25} />
      <path d="M3.5 7.5a4.5 4.5 0 0 0 9 0M8 12v2.5" />
    </Svg>
  );
}

export function MicOffIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <rect x={5.75} y={1.5} width={4.5} height={8} rx={2.25} />
      <path d="M3.5 7.5a4.5 4.5 0 0 0 9 0M8 12v2.5" />
      <path d="M1.5 1.5l13 13" />
    </Svg>
  );
}

export function ScreenShareIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <rect x={1.5} y={2.5} width={13} height={9} rx={1.5} />
      <path d="M5.5 14h5M8 11.5V14M8 9V5M6 7l2-2 2 2" />
    </Svg>
  );
}

export function HangUpIcon(props: SVGProps<SVGSVGElement>) {
  return (
    <Svg {...props}>
      <path d="M1.8 9.6c3.6-3.2 8.8-3.2 12.4 0l-.9 1.9-2.6-.6-.4-1.8a6.6 6.6 0 0 0-4.6 0l-.4 1.8-2.6.6z" />
    </Svg>
  );
}
