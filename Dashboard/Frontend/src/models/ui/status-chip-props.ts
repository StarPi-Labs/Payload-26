export interface StatusChipProps {
    label: string
    value?: boolean | null
    trueLabel?: string
    falseLabel?: string
    unknownLabel?: string
    size?: "xs" | "sm" | "md"
    class?: string
}
