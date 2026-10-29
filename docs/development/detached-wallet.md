# Secure Wallet integration for Detached

Work in progress: no runtime fix implemented yet.

All implementation changes belong in Hermes. Stock KDE, KWin and Linux remain unchanged.

Investigate secure wallet reuse across fresh Detached desktops separately from repeated prompts after Cancel.

Preserve encryption, explicit unlock consent, normal locking, private desktop buses and existing Quit cleanup.

Do not store passwords or derived unlock keys in Hermes, bypass authorization, or silently suppress legitimate requests.

The unresolved design requirement is maintaining access to the wallet and its password prompts when the previous Detached display ends.
