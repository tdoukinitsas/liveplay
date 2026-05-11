## ADDED Requirements

### Requirement: Nuxt 4 runtime

The application SHALL run on Nuxt 4.x with managed Vue and vue-router versions.

#### Scenario: Application builds successfully

- **WHEN** `nuxt generate` is executed
- **THEN** it SHALL produce static output in `.output/public`
- **AND** the output SHALL be loadable by Electron

#### Scenario: Dev server starts

- **WHEN** the developer runs the dev server
- **THEN** Nuxt 4 SHALL start without errors and Electron SHALL load the dev URL

### Requirement: App directory structure

The application source code SHALL reside in the `app/` directory.

#### Scenario: Source files in app directory

- **WHEN** the project is checked out
- **THEN** `app.vue`, `assets/`, `components/`, `composables/`, `utils/`, and `types/` SHALL exist under `app/`

#### Scenario: Nuxt resolves app directory

- **WHEN** Nuxt starts
- **THEN** it SHALL use `app/` as the source directory
- **AND** the `~/` alias SHALL resolve to `app/`

### Requirement: No dead dependencies

The project SHALL NOT include unused or conflicting dependencies.

#### Scenario: No Nuxt 2 types package

- **WHEN** `package.json` is inspected
- **THEN** `@nuxt/types` SHALL NOT be present

#### Scenario: No explicit Vue/vue-router

- **WHEN** `package.json` is inspected
- **THEN** `vue` and `vue-router` SHALL NOT be listed in devDependencies
- **AND** Nuxt SHALL manage their versions internally

### Requirement: SCSS paths resolve correctly

The application styles SHALL load correctly after the directory move.

#### Scenario: Global SCSS loads

- **WHEN** the application starts
- **THEN** `~/assets/styles/main.scss` SHALL resolve to `app/assets/styles/main.scss`
- **AND** styles SHALL render correctly

#### Scenario: SCSS variables available in components

- **WHEN** a component uses SCSS variables
- **THEN** the `additionalData` preprocessor option SHALL inject variables from `app/assets/styles/variables.scss`

### Requirement: Electron build output unchanged

The build output format SHALL remain compatible with the Electron builder configuration.

#### Scenario: Build output location

- **WHEN** `nuxt generate` completes
- **THEN** `.output/public` SHALL contain the SPA files
- **AND** the electron-builder `files` glob (`.output/**/*`) SHALL match the output
