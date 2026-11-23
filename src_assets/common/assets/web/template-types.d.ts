import '@vue/runtime-dom'

declare module '@vue/runtime-dom' {
  interface HTMLAttributes {
    [attribute: `data-bs-${string}`]: string | undefined;
  }
  interface InputHTMLAttributes {
    'true-value'?: string | number | boolean;
    'false-value'?: string | number | boolean;
  }
}
