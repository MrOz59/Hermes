type Theme = 'auto' | 'dark' | 'light';

function isTheme(value: string | null): value is Theme {
    return value === 'auto' || value === 'dark' || value === 'light';
}

const getStoredTheme = (): Theme | null => {
    const theme = localStorage.getItem('theme');
    return isTheme(theme) ? theme : null;
}
const setStoredTheme = (theme: Theme) => localStorage.setItem('theme', theme)

export const getPreferredTheme = (): Theme => {
    const storedTheme = getStoredTheme()
    if (storedTheme) {
        return storedTheme
    }

    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

const setTheme = (theme: Theme) => {
    if (theme === 'auto') {
        document.documentElement.setAttribute(
            'data-bs-theme',
            (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')
        )
    } else {
        document.documentElement.setAttribute('data-bs-theme', theme)
    }
}

export const showActiveTheme = (theme: Theme, focus = false) => {
    const themeSwitcher = document.querySelector<HTMLElement>('#bd-theme')

    if (!themeSwitcher) {
        return
    }

    const themeSwitcherText = document.querySelector('#bd-theme-text')
    const activeThemeIcon = document.querySelector('.theme-icon-active i')
    const btnToActive = document.querySelector(`[data-bs-theme-value="${theme}"]`)
    const classListOfActiveBtn = btnToActive?.querySelector('i')?.classList
    if (!themeSwitcherText || !activeThemeIcon || !btnToActive || !classListOfActiveBtn) return

    document.querySelectorAll('[data-bs-theme-value]').forEach(element => {
        element.classList.remove('active')
        element.setAttribute('aria-pressed', 'false')
    })

    btnToActive.classList.add('active')
    btnToActive.setAttribute('aria-pressed', 'true')
    activeThemeIcon.classList.remove(...activeThemeIcon.classList.values())
    activeThemeIcon.classList.add(...classListOfActiveBtn)
    const themeSwitcherLabel = `${themeSwitcherText.textContent} (${(btnToActive.textContent ?? '').trim()})`
    themeSwitcher.setAttribute('aria-label', themeSwitcherLabel)

    if (focus) {
        themeSwitcher.focus()
    }
}

export function setupThemeToggleListener() {
    document.querySelectorAll('[data-bs-theme-value]')
        .forEach(toggle => {
            toggle.addEventListener('click', () => {
                const theme = toggle.getAttribute('data-bs-theme-value')
                if (!isTheme(theme)) return
                setStoredTheme(theme)
                setTheme(theme)
                showActiveTheme(theme, true)
            })
        })

    showActiveTheme(getPreferredTheme(), false)
}

export function loadAutoTheme() {
    setTheme(getPreferredTheme())

    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
        const storedTheme = getStoredTheme()
        if (storedTheme !== 'light' && storedTheme !== 'dark') {
            setTheme(getPreferredTheme())
        }
    })

    window.addEventListener('DOMContentLoaded', () => {
        showActiveTheme(getPreferredTheme())
    })
}
