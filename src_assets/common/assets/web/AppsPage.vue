<script lang="ts">
import { defineComponent } from 'vue'
import Navbar from './Navbar.vue'
import Checkbox from './Checkbox.vue'
import { Dropdown } from 'bootstrap'

import type { PreparationCommand } from './config-types'

interface CoverCandidate {
  name: string;
  key: string;
  url: string;
  saveUrl: string;
}

interface GameCover {
  id: string | number;
  name: string;
  cover?: { url?: string };
}

/** A game installed in Steam or Lutris, from /api/library. */
interface LibraryGame {
  source: 'steam' | 'lutris';
  id: string;
  name: string;
  cmd: string;
  /** An app already starts it. */
  added: boolean;
}

const newApp = {
  "name": "New App",
  "output": "",
  "cmd": "",
  "exclude-global-prep-cmd": false,
  "exclude-global-state-cmd": false,
  "elevated": false,
  "auto-detach": true,
  "wait-all": true,
  "exit-timeout": 5,
  "prep-cmd": [] as PreparationCommand[],
  "state-cmd": [] as PreparationCommand[],
  "detached": [] as string[],
  "image-path": "",
  "scale-factor": 100,
  "force-scale-factor": false,
  "use-app-identity": false,
  "per-client-app-identity": false,
  "allow-client-commands": true,
  "virtual-display": false,
  "force-virtual-display": false,
  "virtual-display-layout": "auto",
  "unlock-host-session": false,
  "isolation-method": "default",
  "gamescope-steam-mode": "auto",
  "gamescope-args": "",
  "first-frame-timeout": 0,
  "isolate-devices": "auto",
  "isolate-sound": "auto",
  "stop-desktop-steam": "auto",
  "terminate-on-pause": false,
  "stop-detached": true,
  "limit-fps": false,
  "gamepad": ""
}

type Application = typeof newApp & {
  id?: string;
  uuid?: string;
  'working-dir'?: string;
  launching?: boolean;
  dragover?: boolean;
};

function createApplicationDraft(): Application {
  return structuredClone(newApp);
}

interface AppList {
  apps: Application[];
  current_app: string;
  host_name: string;
  host_uuid: string;
}

export default defineComponent({
  components: {
    Navbar,
    Checkbox
  },
  directives: {
    dropdownShow: {
      mounted(element: HTMLElement, binding: { value: EventListener }) {
        element.addEventListener('show.bs.dropdown', binding.value)
      },
      beforeUnmount(element: HTMLElement, binding: { value: EventListener }) {
        element.removeEventListener('show.bs.dropdown', binding.value)
      },
    },
  },
  data() {
    return {
      apps: [] as Application[],
      showEditForm: false,
      actionDisabled: false,
      editForm: createApplicationDraft(),
      detachedCmd: "",
      coverSearching: false,
      coverFinderBusy: false,
      commandGroups: ['prep', 'state'] as const,
      isolationSettings: ['isolate-devices', 'isolate-sound'] as const,
      coverCandidates: [] as CoverCandidate[],
      platform: "",
      currentApp: "",
      draggingApp: -1,
      hostName: "",
      hostUUID: "",
      listReordered: false,
      showLibrary: false,
      libraryLoading: false,
      libraryGames: [] as LibraryGame[],
      librarySelected: [] as LibraryGame[]
    };
  },
  computed: {
    librarySelectable(): LibraryGame[] {
      return this.libraryGames.filter(game => !game.added);
    }
  },
  created() {
    this.loadApps();

    fetch("./api/config", {
      credentials: 'include'
    })
      .then(r => r.json())
      .then(r => this.platform = r.platform);
  },
  methods: {
    /** An app already starts this game when one of its commands opens it by ID. */
    libraryGameAdded(game: Omit<LibraryGame, 'added'>): boolean {
      const uri = game.source === 'steam' ? `steam://rungameid/${game.id}` : `lutris:rungameid/${game.id}`;
      const ends = (cmd: string) => {
        const at = (cmd || '').indexOf(uri);
        return at >= 0 && !/[0-9]/.test(cmd.charAt(at + uri.length));
      };
      return this.apps.some(app => ends(app.cmd) || (app.detached || []).some(ends));
    },
    openLibrary() {
      this.showLibrary = true;
      this.libraryLoading = true;
      this.libraryGames = [];
      this.librarySelected = [];
      fetch("./api/library", {
        credentials: 'include'
      })
        .then(r => r.json())
        .then((r: { games?: Omit<LibraryGame, 'added'>[] }) => {
          this.libraryGames = (r.games || []).map(game => ({...game, added: this.libraryGameAdded(game)}));
        })
        .finally(() => this.libraryLoading = false);
    },
    selectAllLibrary(checked: boolean) {
      this.librarySelected = checked ? [...this.librarySelectable] : [];
    },
    async importLibrary() {
      this.actionDisabled = true;
      const failed: string[] = [];
      try {
        // One at a time: each save rewrites the apps file.
        for (const game of this.librarySelected) {
          const r = await fetch("./api/apps", {
            credentials: 'include',
            headers: {
              'Content-Type': 'application/json'
            },
            method: 'POST',
            body: JSON.stringify({...createApplicationDraft(), name: game.name, detached: [game.cmd]}),
          }).then(r => r.json()).catch(() => ({status: false}));
          if (!r.status) {
            failed.push(game.name);
          }
        }
      } finally {
        this.actionDisabled = false;
      }
      if (failed.length > 0) {
        alert(this.$t('apps.save_failed') + failed.join(', '));
      }
      this.showLibrary = false;
      this.loadApps();
    },
    onDragStart(e: DragEvent, idx: number) {
      if (this.showEditForm) {
        e.preventDefault();
        return;
      }

      this.draggingApp = idx;
      this.apps.push({ ...newApp, name: "" })
    },
    onDragEnter(e: DragEvent, app: Application) {
      if (this.draggingApp < 0) {
        return;
      }
      e.preventDefault();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "move";
      app.dragover = true;
    },
    onDragOver(e: DragEvent) {
      if (this.draggingApp < 0) {
        return;
      }
      e.preventDefault();
      if (e.dataTransfer) e.dataTransfer.dropEffect = "move";
    },
    onDragLeave(app: Application) {
      app.dragover = false;
    },
    onDragEnd() {
      this.draggingApp = -1;
      this.apps.pop();
    },
    onDrop(e: DragEvent, app: Application, idx: number) {
      app.dragover = false;
      if (this.draggingApp < 0) {
        return;
      }
      e.preventDefault();

      if (idx === this.draggingApp || idx - 1 === this.draggingApp) {
        return;
      }

      const draggedApp = this.apps[this.draggingApp];
      this.apps.splice(this.draggingApp, 1);

      if (idx > this.draggingApp) {
        idx -= 1;
      }

      this.apps.splice(idx, 0, draggedApp);

      this.listReordered = true;
    },
    alphabetizeApps() {
      let orderStat = 0;
      this.apps.sort((a: Application, b: Application) => {
        const result = a.name.localeCompare(b.name);
        orderStat += result;
        return result;
      });
      this.listReordered = orderStat !== this.apps.length - 1;
      if (!this.listReordered) {
        alert(this.$t('apps.already_ordered'));
      }
    },
    saveOrder() {
      this.actionDisabled = true;

      const reorderedUUIDs = this.apps.reduce((reordered: string[], i: Application) => {
        if (i.uuid) {
          reordered.push(i.uuid)
        }
        return reordered
      }, [])

      fetch("./api/apps/reorder", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify({order: reorderedUUIDs})
      })
      .then(r => r.json())
      .then((r) => {
        if (!r.status) {
          alert(this.$t("apps.reorder_failed") + r.error);
        }
      })
      .finally(() => {
        return this.loadApps();
      })
      .finally(() => {
        this.actionDisabled = false;
      })

    },
    loadApps() {
      return fetch("./api/apps", {
        credentials: 'include'
      })
      .then(r => r.json())
      .then((r: AppList) => {
        this.apps = r.apps.filter(i => i.uuid).map(i => ({...i, launching: false, dragover: false}));
        this.currentApp = r.current_app;
        this.hostName = r.host_name;
        this.hostUUID = r.host_uuid;
        this.listReordered = false;
      });
    },
    newApp() {
      this.editForm = createApplicationDraft();
      this.showEditForm = true;
    },
    launchApp(event: MouseEvent, app: Application) {
      if (this.actionDisabled) return;
      const isLocalHost = ['localhost', '127.0.0.1', '[::1]'].indexOf(location.hostname) >= 0

      if (!isLocalHost && confirm(this.$t('apps.launch_local_client'))) {
        const link = document.createElement('a');
        if (!(event.currentTarget instanceof HTMLAnchorElement)) return;
        link.href = event.currentTarget.href;
        link.click();
        return;
      }

      if (confirm(this.$t('apps.launch_warning'))) {
        this.actionDisabled = true;
        fetch("./api/apps/launch", {
          credentials: 'include',
          headers: {
            'Content-Type': 'application/json'
          },
          method: 'POST',
          body: JSON.stringify({ uuid: app.uuid })
        })
        .then(r => r.json())
        .then(r => {
          if (!r.status) {
            alert(this.$t('apps.launch_failed') + r.error);
          }
        })
        .finally(() => {
          this.actionDisabled = false;
          this.loadApps()
        });
      }
    },
    closeApp() {
      if (confirm(this.$t('apps.close_warning'))) {
        this.actionDisabled = true;
        fetch("./api/apps/close", {
          credentials: 'include',
          headers: {
            'Content-Type': 'application/json'
          },
          method: 'POST',
        })
        .then((r) => r.json())
        .then((r) => {
          if (!r.status) {
            alert("apps.close_failed")
          }
        })
        .finally(() => {
          this.actionDisabled = false;
          this.loadApps()
        });
      }
    },
    editApp(app: Application) {
      this.editForm = Object.assign({}, newApp, JSON.parse(JSON.stringify(app)));
      this.showEditForm = true;
    },
    exportLauncherFile(app: Application) {
      const link = document.createElement('a');
      const fileContent = `# Artemis app entry
# Exported by Hermes
# https://github.com/MrOz59/Hermes

[host_uuid] ${this.hostUUID}
[host_name] ${this.hostName}
[app_uuid] ${app.uuid}
[app_name] ${app.name}
`;
      link.href = `data:text/plain;charset=utf-8,${encodeURIComponent(fileContent)}`;
      link.download = `${app.name}.art`;
      link.click();
    },
    showDeleteForm(app: Application) {
      const resp = confirm(
        "Are you sure to delete " + app.name + "?"
      );
      if (resp) {
        this.actionDisabled = true;
        fetch("./api/apps/delete", {
          credentials: 'include',
          headers: {
            'Content-Type': 'application/json'
          },
          method: 'POST',
          body: JSON.stringify({ uuid: app.uuid })
        }).then((r) => r.json())
        .then((r) => {
          if (!r.status) {
            alert("Delete failed! " + r.error);
          }
        })
        .finally(() => {
          this.actionDisabled = false;
          this.loadApps();
        });
      }
    },
    addCmd(cmdArr: PreparationCommand[], idx: number) {
      const template = {
        do: "",
        undo: ""
      };

      if (idx < 0) {
        cmdArr.push(template);
      } else {
        cmdArr.splice(idx, 0, template);
      }
    },
    showCoverFinder() {
      this.coverCandidates = [];
      this.coverSearching = true;
      const ref = this.$refs.coverFinderDropdown;
      if (!(ref instanceof HTMLElement)) {
        console.error("Ref not found!");
        return;
      }
      Dropdown.getOrCreateInstance(ref).show();
      function getSearchBucket(name: string) {
        let bucket = name.substring(0, Math.min(name.length, 2)).toLowerCase().replaceAll(/[^a-z\d]/g, '');
        if (!bucket) {
          return '@';
        }
        return bucket;
      }

      function searchCovers(name: string): Promise<CoverCandidate[]> {
        if (!name) {
          return Promise.resolve([]);
        }
        let searchName = name.replaceAll(/\s+/g, '.').toLowerCase();

        // Use raw.githubusercontent.com to avoid CORS issues as we migrate the CNAME
        let dbUrl = "https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages";
        let bucket = getSearchBucket(name);
        return fetch(`${dbUrl}/buckets/${bucket}.json`).then(function (r) {
          if (!r.ok) throw new Error("Failed to search covers");
          return r.json();
        }).then((maps: Record<string, { name: string }>) => Promise.all(Object.keys(maps).map(id => {
          let item = maps[id];
          if (item.name.replaceAll(/\s+/g, '.').toLowerCase().startsWith(searchName)) {
            return fetch(`${dbUrl}/games/${id}.json`).then(function (r): Promise<GameCover> {
              return r.json();
            }).catch(() => null);
          }
          return null;
        }).filter(item => item)))
          .then(results => results
            .map(game => {
              const thumb = game?.cover?.url;
              if (!game || !thumb) return null;
              const dotIndex = thumb.lastIndexOf('.');
              const slashIndex = thumb.lastIndexOf('/');
              if (dotIndex < 0 || slashIndex < 0) {
                return null;
              }
              const slug = thumb.substring(slashIndex + 1, dotIndex);
              return {
                name: game.name,
                key: `igdb_${game.id}`,
                url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
                saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
              }
            }).filter(item => item !== null));
      }

      searchCovers(this.editForm?.name.trim() ?? "")
        .then(list => this.coverCandidates = list)
        .finally(() => this.coverSearching = false);
    },
    closeCoverFinder() {
      const ref = this.$refs.coverFinderDropdown;
      if (!(ref instanceof HTMLElement)) {
        return;
      }
      const dropdown = Dropdown.getInstance(ref);
      if (!dropdown) {
        return;
      }
      dropdown.hide();
    },
    useCover(cover: CoverCandidate) {
      this.coverFinderBusy = true;
      fetch("./api/covers/upload", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify({
          key: cover.key,
          url: cover.saveUrl,
        })
      }).then(r => {
        if (!r.ok) throw new Error("Failed to download covers");
        return r.json();
      }).then((body: { path: string }) => {
        if (this.editForm) this.editForm["image-path"] = body.path;
      })
        .then(() => this.closeCoverFinder())
        .finally(() => this.coverFinderBusy = false);
    },
    save() {
      if (!this.editForm) return;
      this.editForm.name = this.editForm.name.trim();
      if (!this.editForm.name) {
        return;
      }
      this.editForm["exit-timeout"] = Number.parseInt(String(this.editForm["exit-timeout"]), 10) || 5
      this.editForm["scale-factor"] = Number.parseInt(String(this.editForm["scale-factor"]), 10) || 100
      this.editForm["first-frame-timeout"] = Math.min(600, Math.max(0, Number.parseInt(String(this.editForm["first-frame-timeout"]), 10) || 0))
      this.editForm["image-path"] = this.editForm["image-path"].toString().trim().replace(/"/g, '');
      delete this.editForm.id;
      delete this.editForm.launching;
      delete this.editForm.dragover;
      fetch("./api/apps", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify(this.editForm),
      }).then((r) => r.json())
      .then((r) => {
        if (!r.status) {
          alert(this.$t('apps.save_failed') + r.error);
          throw new Error(`App save failed: ${r.error}`);
        }
      })
      .then(() => {
        this.showEditForm = false;
        this.loadApps();
      });
    },
  },
});
</script>

<template>
  <Navbar></Navbar>
  <div class="container">
    <div class="my-4" v-if="!showEditForm">
        <h1>{{ $t('apps.applications_title') }}</h1>
        <div>{{ $t('apps.applications_desc') }}</div>
        <div>{{ $t('apps.applications_reorder_desc') }}</div>
        <div class="pre-wrap">{{ $t('apps.applications_tips') }}</div>
    </div>
    <div class="my-4" v-else>
        <h1>{{ editForm.name || "&lt; NO NAME &gt;"}}</h1>
        <div>
          <button @click="showEditForm = false" class="btn btn-secondary me-2 mt-2">
            <i class="fas fa-xmark"></i> {{ $t('_common.cancel') }}
          </button>
          <button class="btn btn-primary me-2 mt-2" :disabled="actionDisabled || !editForm.name.trim()" @click="save">
            <i class="fas fa-floppy-disk"></i> {{ $t('_common.save') }}
          </button>
          <button class="btn btn-success mt-2" @click="exportLauncherFile(editForm)" :disabled="!editForm.uuid">
            <i class="fas fa-arrow-up-from-bracket"></i> {{ $t('apps.export_launcher_file') }}
          </button>
        </div>
    </div>
    <div class="card p-4" v-if="!showEditForm && showLibrary">
      <h5>{{ $t('apps.library_title') }}</h5>
      <p class="text-muted">{{ $t('apps.library_desc') }}</p>
      <div v-if="libraryLoading" class="text-center my-4">
        <span class="spinner-border position-static" role="status"></span>
      </div>
      <div v-else-if="libraryGames.length === 0" class="alert alert-info">
        {{ $t('apps.library_empty') }}
      </div>
      <table class="table" v-else>
        <thead>
          <tr>
            <th scope="col" style="width: 2em">
              <input class="form-check-input" type="checkbox" :checked="librarySelectable.length > 0 && librarySelected.length === librarySelectable.length"
                     @change="selectAllLibrary(($event.target as HTMLInputElement).checked)" :disabled="librarySelectable.length === 0" />
            </th>
            <th scope="col">{{ $t('apps.name') }}</th>
            <th scope="col">{{ $t('apps.library_source') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="game in libraryGames" :key="game.source + ':' + game.id">
            <td>
              <input class="form-check-input" type="checkbox" :value="game" v-model="librarySelected" :disabled="game.added" />
            </td>
            <td>{{ game.name }}<span v-if="game.added" class="text-muted ms-2">({{ $t('apps.library_added') }})</span></td>
            <td>{{ game.source === 'steam' ? 'Steam' : 'Lutris' }}</td>
          </tr>
        </tbody>
      </table>
      <div class="mt-2">
        <button class="btn btn-secondary me-2" @click="showLibrary = false" :disabled="actionDisabled">
          <i class="fas fa-xmark"></i> {{ $t('_common.cancel') }}
        </button>
        <button class="btn btn-primary" @click="importLibrary" :disabled="actionDisabled || librarySelected.length === 0">
          <i class="fas fa-plus"></i> {{ $t('apps.library_import', { count: librarySelected.length }) }}
        </button>
      </div>
    </div>
    <div class="card p-4" v-if="!showEditForm && !showLibrary">
      <table class="table">
        <thead>
          <tr>
            <th scope="col">{{ $t('apps.name') }}</th>
            <th scope="col" class="text-end actions-col">{{ $t('apps.actions') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="(app,i) in apps"
            :key="app.uuid"
            :class="{dragover: app.dragover}"
            draggable="true"
            @dragstart="onDragStart($event, i)"
            @dragenter="onDragEnter($event, app)"
            @dragover="onDragOver($event)"
            @dragleave="onDragLeave(app)"
            @dragend="onDragEnd()"
            @drop="onDrop($event, app, i)"
          >
            <td @dblclick="exportLauncherFile(app)">{{app.name || ' '}}</td>
            <td v-if="app.uuid" class="text-end">
              <button class="btn btn-primary me-1" :disabled="actionDisabled" @click="editApp(app)">
                <i class="fas fa-edit"></i>
              </button>
              <button class="btn btn-danger me-1" :disabled="actionDisabled" @click="showDeleteForm(app)">
                <i class="fas fa-trash"></i>
              </button>
              <button class="btn btn-warning" :disabled="actionDisabled" @click="closeApp()" v-if="currentApp === app.uuid">
                <i class="fas fa-stop"></i>
              </button>
              <a class="btn btn-success" :aria-disabled="actionDisabled" :class="{ disabled: actionDisabled }" @click.prevent="launchApp($event, app)" :href="'art://launch?host_uuid=' + hostUUID + '&host_name=' + hostName + '&app_uuid=' + app.uuid + '&app_name=' + app.name" v-else>
                <i class="fas fa-play"></i>
              </a>
            </td>
            <td v-else></td>
          </tr>
        </tbody>
      </table>
      <div class="mt-2">
        <button class="btn btn-primary" @click="newApp" :disabled="actionDisabled">
          <i class="fas fa-plus"></i> {{ $t('apps.add_new') }}
        </button>
        <button class="btn btn-outline-primary ms-2" @click="openLibrary" :disabled="actionDisabled" v-if="platform === 'linux'">
          <i class="fas fa-gamepad"></i> {{ $t('apps.library_open') }}
        </button>
        <button class="btn btn-secondary float-end" @click="alphabetizeApps" :disabled="actionDisabled" v-if="!listReordered">
          <i class="fas fa-sort-alpha-up"></i> {{ $t('apps.alphabetize') }}
        </button>
        <button class="btn btn-warning float-end" @click="saveOrder" :disabled="actionDisabled" v-if="listReordered">
          <i class="fas fa-floppy-disk"></i> {{ $t('apps.save_order') }}
        </button>
      </div>
    </div>
    <div class="edit-form card mt-2" v-else-if="showEditForm">
      <div class="p-4">
        <!-- Application Name -->
        <div class="mb-3">
          <label for="appName" class="form-label">{{ $t('apps.app_name') }}</label>
          <div class="input-group dropup">
            <input type="text" class="form-control" id="appName" aria-describedby="appNameHelp" v-model="editForm.name" />
            <button class="btn btn-secondary dropdown-toggle" type="button" id="findCoverToggle"
              aria-expanded="false" @click="showCoverFinder" ref="coverFinderDropdown">
              {{ $t('apps.find_cover') }} (Online)
            </button>
            <div class="dropdown-menu dropdown-menu-end w-50 cover-finder overflow-hidden"
              aria-labelledby="findCoverToggle">
              <div class="modal-header px-2">
                <h4 class="modal-title">{{ $t('apps.covers_found') }}</h4>
                <button type="button" class="btn-close mr-2" aria-label="Close" @click="closeCoverFinder"></button>
              </div>
              <div class="modal-body cover-results px-3 pt-3" :class="{ busy: coverFinderBusy }">
                <div class="row">
                  <div v-if="coverSearching" class="col-12 col-sm-6 col-lg-4 mb-3">
                    <div class="cover-container">
                      <div class="spinner-border" role="status">
                        <span class="visually-hidden">{{ $t('apps.loading') }}</span>
                      </div>
                    </div>
                  </div>
                  <div v-for="(cover,i) in coverCandidates" :key="'i'" class="col-12 col-sm-6 col-lg-4 mb-3"
                    @click="useCover(cover)">
                    <div class="cover-container result">
                      <img class="rounded" :src="cover.url" />
                    </div>
                    <label class="d-block text-nowrap text-center text-truncate">
                      {{cover.name}}
                    </label>
                  </div>
                </div>
              </div>
            </div>
          </div>
          <div id="appNameHelp" class="form-text">{{ $t('apps.app_name_desc') }}</div>
        </div>
        <!-- Application Image -->
        <div class="mb-3">
          <label for="appImagePath" class="form-label">{{ $t('apps.image') }}</label>
          <input type="text" class="form-control monospace" id="appImagePath" aria-describedby="appImagePathHelp"
            v-model="editForm['image-path']" />
          <div id="appImagePathHelp" class="form-text">{{ $t('apps.image_desc') }}</div>
        </div>
        <!-- gamepad override -->
        <div class="mb-3">
          <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
          <select id="gamepad" class="form-select" v-model="editForm.gamepad">
            <option value="">{{ $t('_common.default_global') }}</option>
            <option value="disabled">{{ $t('_common.disabled') }}</option>
            <option value="auto">{{ $t('_common.auto') }}</option>

            <template v-if="platform === 'linux'">
              <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
              <option value="switch">{{ $t("config.gamepad_switch") }}</option>
              <option value="xone">{{ $t("config.gamepad_xone") }}</option>
            </template>


          </select>
          <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
        </div>
        <!-- command -->
        <div class="mb-3">
          <label for="appCmd" class="form-label">{{ $t('apps.cmd') }}</label>
          <input type="text" class="form-control monospace" id="appCmd" aria-describedby="appCmdHelp"
            v-model="editForm.cmd" />
          <div id="appCmdHelp" class="form-text">
            {{ $t('apps.cmd_desc') }}<br>
            <b>{{ $t('_common.note') }}</b> {{ $t('apps.cmd_note') }}
          </div>
        </div>
        <!-- detached -->
        <div class="mb-3">
          <label class="form-label">{{ $t('apps.detached_cmds') }}</label>
          <div v-for="(c,i) in editForm.detached" class="d-flex justify-content-between my-2">
            <input type="text" v-model="editForm.detached[i]" class="form-control monospace">
            <button class="btn btn-danger mx-2" @click="editForm.detached.splice(i,1)">
              <i class="fas fa-trash"></i>
            </button>
            <button class="btn btn-success" @click="editForm.detached.splice(i, 0, '')">
              <i class="fas fa-plus"></i>
            </button>
          </div>
          <div class="d-flex justify-content-between">
            <button class="btn btn-success" @click="editForm.detached.push('');">
              <i class="fas fa-plus mr-1"></i> {{ $t('apps.detached_cmds_add') }}
            </button>
          </div>
          <div class="form-text">
            {{ $t('apps.detached_cmds_desc') }}<br>
            <b>{{ $t('_common.note') }}</b> {{ $t('apps.detached_cmds_note') }}
          </div>
        </div>
        <!-- allow client commands -->
        <Checkbox class="mb-3"
                  id="clientCommands"
                  label="apps.allow_client_commands"
                  desc="apps.allow_client_commands_desc"
                  v-model="editForm['allow-client-commands']"
                  default="true"
        ></Checkbox>
        <!-- prep and state-cmd -->
        <template v-for="type in commandGroups" :key="type">
          <Checkbox class="mb-3"
                    :id="'excludeGlobal_' + type"
                    :label="'apps.global_' + type + '_name'"
                    :desc="'apps.global_' + type + '_desc'"
                    v-model="editForm[`exclude-global-${type}-cmd`]"
                    default="true"
                    inverse-values
          ></Checkbox>
          <div class="mb-3">
            <label class="form-label">{{ $t('apps.cmd_' + type + '_name') }}</label>
            <div class="form-text pre-wrap">{{ $t('apps.cmd_' + type + '_desc') }}</div>
            <table class="table" v-if="editForm[`${type}-cmd`].length > 0">
              <thead>
                <tr>
                  <th scope="col"><i class="fas fa-play"></i> {{ $t('_common.do_cmd') }}</th>
                  <th scope="col"><i class="fas fa-undo"></i> {{ $t('_common.undo_cmd') }}</th>
                  <th scope="col"></th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(c, i) in editForm[`${type}-cmd`]">
                  <td>
                    <input type="text" class="form-control monospace" v-model="c.do" />
                  </td>
                  <td>
                    <input type="text" class="form-control monospace" v-model="c.undo" />
                  </td>
                  <td class="text-end">
                    <button class="btn btn-danger mx-2" @click="editForm[`${type}-cmd`].splice(i,1)">
                      <i class="fas fa-trash"></i>
                    </button>
                    <button class="btn btn-success" @click="addCmd(editForm[`${type}-cmd`], i)">
                      <i class="fas fa-plus"></i>
                    </button>
                  </td>
                </tr>
              </tbody>
            </table>
            <div class="d-flex justify-content-start mb-3 mt-3">
              <button class="btn btn-success" @click="addCmd(editForm[`${type}-cmd`], -1)">
                <i class="fas fa-plus mr-1"></i> {{ $t('apps.add_cmds') }}
              </button>
            </div>
          </div>
        </template>
        <!-- working dir -->
        <div class="mb-3">
          <label for="appWorkingDir" class="form-label">{{ $t('apps.working_dir') }}</label>
          <input type="text" class="form-control monospace" id="appWorkingDir" aria-describedby="appWorkingDirHelp"
            v-model="editForm['working-dir']" />
          <div id="appWorkingDirHelp" class="form-text">{{ $t('apps.working_dir_desc') }}</div>
        </div>
        <!-- output -->
        <div class="mb-3">
          <label for="appOutput" class="form-label">{{ $t('apps.output_name') }}</label>
          <input type="text" class="form-control monospace" id="appOutput" aria-describedby="appOutputHelp"
            v-model="editForm.output" />
          <div id="appOutputHelp" class="form-text">{{ $t('apps.output_desc') }}</div>
        </div>
        <!-- auto-detach -->
        <Checkbox class="mb-3"
                  id="autoDetach"
                  label="apps.auto_detach"
                  desc="apps.auto_detach_desc"
                  v-model="editForm['auto-detach']"
                  default="true"
        ></Checkbox>
        <!-- wait for all processes -->
        <Checkbox class="mb-3"
                  id="waitAll"
                  label="apps.wait_all"
                  desc="apps.wait_all_desc"
                  v-model="editForm['wait-all']"
                  default="true"
        ></Checkbox>
        <!-- terminate on pause -->
        <Checkbox class="mb-3"
                  id="terminateOnPause"
                  label="apps.terminate_on_pause"
                  desc="apps.terminate_on_pause_desc"
                  v-model="editForm['terminate-on-pause']"
                  default="false"
        ></Checkbox>
        <!-- cap the application at the stream's frame rate -->
        <Checkbox v-if="platform === 'linux'"
                  class="mb-3"
                  id="limitFps"
                  label="apps.limit_fps"
                  desc="apps.limit_fps_desc"
                  v-model="editForm['limit-fps']"
                  default="false"
        ></Checkbox>
        <!-- stop detached commands with the app -->
        <Checkbox v-if="platform === 'linux'"
                  class="mb-3"
                  id="stopDetached"
                  label="apps.stop_detached"
                  desc="apps.stop_detached_desc"
                  v-model="editForm['stop-detached']"
                  default="true"
        ></Checkbox>
        <!-- resolution scale -->
        <div class="mb-3">
          <label for="scaleFactor" class="form-label">{{ $t('apps.resolution_scale_factor') }}</label>
          <input type="number" class="form-control monospace" id="scaleFactor" aria-describedby="scaleFactorHelp"
                 v-model="editForm['scale-factor']" min="1" max="500" placeholder="100" />
          <div id="scaleFactorHelp" class="form-text">{{ $t('apps.resolution_scale_factor_desc') }}</div>
        </div>
        <Checkbox class="mb-3"
                  id="forceScaleFactor"
                  label="apps.force_scale_factor"
                  desc="apps.force_scale_factor_desc"
                  v-model="editForm['force-scale-factor']"
                  default="false"
        ></Checkbox>
        <!-- exit timeout -->
        <div class="mb-3">
          <label for="exitTimeout" class="form-label">{{ $t('apps.exit_timeout') }}</label>
          <input type="number" class="form-control monospace" id="exitTimeout" aria-describedby="exitTimeoutHelp"
                 v-model="editForm['exit-timeout']" min="0" placeholder="5" />
          <div id="exitTimeoutHelp" class="form-text">{{ $t('apps.exit_timeout_desc') }}</div>
        </div>
        <!-- use virtual display -->
        <Checkbox class="mb-3"
                  id="virtualDisplay"
                  label="apps.virtual_display"
                  desc="apps.virtual_display_desc"
                  v-model="editForm['virtual-display']"
                  default="false"
        ></Checkbox>
        <!-- force the virtual display setting on every client -->
        <Checkbox class="mb-3"
                  id="forceVirtualDisplay"
                  label="apps.force_virtual_display"
                  desc="apps.force_virtual_display_desc"
                  v-model="editForm['force-virtual-display']"
                  default="false"
        ></Checkbox>
        <!-- virtual display layout -->
        <div class="mb-3" v-if="platform === 'linux' && editForm['virtual-display']">
          <label for="virtualDisplayLayout" class="form-label">{{ $t('apps.virtual_display_layout') }}</label>
          <select id="virtualDisplayLayout" class="form-select" v-model="editForm['virtual-display-layout']">
            <option value="auto">{{ $t('apps.virtual_display_layout_auto') }}</option>
            <option value="extend">{{ $t('apps.virtual_display_layout_extend') }}</option>
            <option value="primary">{{ $t('apps.virtual_display_layout_primary') }}</option>
            <option value="detached">{{ $t('apps.virtual_display_layout_detached') }}</option>
            <option value="mirror">{{ $t('apps.virtual_display_layout_mirror') }}</option>
            <option value="exclusive">{{ $t('apps.virtual_display_layout_exclusive') }}</option>
          </select>
          <div class="form-text">{{ $t('apps.virtual_display_layout_desc') }}</div>
        </div>
        <!-- what this app runs in; overrides the global isolation method -->
        <div class="mb-3" v-if="platform === 'linux'">
          <label for="isolationMethod" class="form-label">{{ $t('apps.isolation_method') }}</label>
          <select id="isolationMethod" class="form-select" v-model="editForm['isolation-method']">
            <option value="default">{{ $t('apps.isolation_method_default') }}</option>
            <option value="gamescope">{{ $t('apps.isolation_method_gamescope') }}</option>
            <option value="kwin_virtual">{{ $t('apps.isolation_method_kwin_virtual') }}</option>
          </select>
          <div class="form-text">{{ $t('apps.isolation_method_desc') }}</div>
        </div>
        <!-- optionally unlock the existing host desktop session -->
        <Checkbox v-if="platform === 'linux'"
                  class="mb-3"
                  id="unlockHostSession"
                  label="apps.unlock_host_session"
                  desc="apps.unlock_host_session_desc"
                  v-model="editForm['unlock-host-session']"
                  default="false"
        ></Checkbox>
        <!-- gamescope flags for sessions isolated with gamescope -->
        <div class="mb-3" v-if="platform === 'linux' && editForm['isolation-method'] !== 'kwin_virtual'">
          <label for="gamescopeSteamMode" class="form-label">{{ $t('apps.gamescope_steam_mode') }}</label>
          <select id="gamescopeSteamMode" class="form-select" v-model="editForm['gamescope-steam-mode']">
            <option value="auto">{{ $t('apps.gamescope_steam_mode_auto') }}</option>
            <option value="on">{{ $t('apps.gamescope_steam_mode_on') }}</option>
            <option value="off">{{ $t('apps.gamescope_steam_mode_off') }}</option>
          </select>
          <div class="form-text">{{ $t('apps.gamescope_steam_mode_desc') }}</div>
        </div>
        <div class="mb-3" v-if="platform === 'linux' && editForm['isolation-method'] !== 'kwin_virtual'">
          <label for="gamescopeArgs" class="form-label">{{ $t('apps.gamescope_args') }}</label>
          <input type="text" class="form-control monospace" id="gamescopeArgs" v-model="editForm['gamescope-args']"
                 placeholder="--expose-wayland --force-windows-fullscreen" />
          <div class="form-text">{{ $t('apps.gamescope_args_desc') }}</div>
        </div>
        <!-- how long an isolated session has to show its first frame -->
        <div class="mb-3" v-if="platform === 'linux'">
          <label for="firstFrameTimeout" class="form-label">{{ $t('apps.session_first_frame_timeout') }}</label>
          <input type="number" class="form-control monospace" id="firstFrameTimeout" aria-describedby="firstFrameTimeoutHelp"
                 v-model="editForm['first-frame-timeout']" min="0" max="600" placeholder="0" />
          <div id="firstFrameTimeoutHelp" class="form-text">{{ $t('apps.session_first_frame_timeout_desc') }}</div>
        </div>
        <!-- what isolated sessions see of the host's /dev -->
        <div class="mb-3" v-if="platform === 'linux'">
          <label for="stopDesktopSteam" class="form-label">{{ $t('apps.stop_desktop_steam') }}</label>
          <select id="stopDesktopSteam" class="form-select" v-model="editForm['stop-desktop-steam']">
            <option value="auto">{{ $t('apps.stop_desktop_steam_auto') }}</option>
            <option value="on">{{ $t('apps.isolate_on_plain') }}</option>
            <option value="off">{{ $t('apps.isolate_off') }}</option>
          </select>
          <div class="form-text">{{ $t('apps.stop_desktop_steam_desc') }}</div>
        </div>
        <template v-for="field in isolationSettings" :key="field">
          <div class="mb-3" v-if="platform === 'linux'">
            <label :for="field" class="form-label">{{ $t('apps.' + field.replace('-', '_')) }}</label>
            <select :id="field" class="form-select" v-model="editForm[field]">
              <option value="auto">{{ $t('apps.isolate_auto') }}</option>
              <option value="on">{{ $t('apps.isolate_on') }}</option>
              <option value="off">{{ $t('apps.isolate_off') }}</option>
            </select>
            <div class="form-text">{{ $t('apps.' + field.replace('-', '_') + '_desc') }}</div>
          </div>
        </template>
        <!-- use app identity -->
        <Checkbox class="mb-3"
                  id="useAppIdentity"
                  label="apps.use_app_identity"
                  desc="apps.use_app_identity_desc"
                  v-model="editForm['use-app-identity']"
                  default="false"
        ></Checkbox>
        <!-- per-client app identity -->
        <Checkbox class="mb-3"
                  v-if="editForm['use-app-identity']"
                  id="perClientAppIdentity"
                  label="apps.per_client_app_identity"
                  desc="apps.per_client_app_identity_desc"
                  v-model="editForm['per-client-app-identity']"
                  default="false"
        ></Checkbox>
        <div class="env-hint alert alert-info overflow-auto">
          <div class="form-text">
            <h4>{{ $t('apps.env_vars_about') }}</h4>
            {{ $t('apps.env_vars_desc') }}
          </div>
          <table class="env-table">
            <tr>
              <td><b>{{ $t('apps.env_var_name') }}</b></td>
              <td><b></b></td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_APP_ID</td>
              <td>{{ $t('apps.env_app_id') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_APP_NAME</td>
              <td>{{ $t('apps.env_app_name') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_APP_UUID</td>
              <td>{{ $t('apps.env_app_uuid') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_APP_STATUS</td>
              <td>{{ $t('apps.env_app_status') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_UUID</td>
              <td>{{ $t('apps.env_client_uuid') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_NAME</td>
              <td>{{ $t('apps.env_client_name') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_WIDTH</td>
              <td>{{ $t('apps.env_client_width') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_HEIGHT</td>
              <td>{{ $t('apps.env_client_height') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_FPS</td>
              <td>{{ $t('apps.env_client_fps') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_HDR</td>
              <td>{{ $t('apps.env_client_hdr') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_GCMAP</td>
              <td>{{ $t('apps.env_client_gcmap') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_HOST_AUDIO</td>
              <td>{{ $t('apps.env_client_host_audio') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">HERMES_CLIENT_ENABLE_SOPS</td>
              <td>{{ $t('apps.env_client_enable_sops') }}</td>
            </tr>
            <tr>
              <td style="font-family: monospace">APOLLO_CLIENT_AUDIO_CONFIGURATION</td>
              <td>{{ $t('apps.env_client_audio_config') }}</td>
            </tr>
          </table>

          <template v-if="platform === 'linux'">
            <div class="form-text"><b>{{ $t('apps.env_xrandr_example') }}</b>
              <pre>sh -c "xrandr --output HDMI-1 --mode \"${HERMES_CLIENT_WIDTH}x${HERMES_CLIENT_HEIGHT}\" --rate ${HERMES_CLIENT_FPS}"</pre>
            </div>
          </template>

          <div class="form-text">
            <a
              href="https://github.com/MrOz59/Hermes/blob/main/docs/app_examples.md"
              target="_blank"
              class="text-decoration-none"
            >
              {{ $t('_common.see_more') }}
            </a>
          </div>
        </div>

        <div class="alert alert-info">
          <i class="fa-solid fa-xl fa-circle-info"></i> {{ $t('apps.env_sunshine_compatibility') }}
        </div>

        <!-- Save buttons -->
        <div class="d-flex">
          <button @click="showEditForm = false" class="btn btn-secondary me-2">
            <i class="fas fa-xmark"></i> {{ $t('_common.cancel') }}
          </button>
          <button class="btn btn-primary" :disabled="actionDisabled || !editForm.name.trim()" @click="save">
            <i class="fas fa-floppy-disk"></i> {{ $t('_common.save') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<style>
.precmd-head {
          width: 200px;
        }

        .monospace {
          font-family: monospace;
        }

        .cover-finder {}

        .cover-finder .cover-results {
          max-height: 400px;
          overflow-x: hidden;
          overflow-y: auto;
        }

        .cover-finder .cover-results.busy * {
          cursor: wait !important;
          pointer-events: none;
        }

        .cover-container {
          padding-top: 133.33%;
          position: relative;
        }

        .cover-container.result {
          cursor: pointer;
        }

        .spinner-border {
          position: absolute;
          left: 0;
          top: 0;
          right: 0;
          bottom: 0;
          margin: auto;
        }

        .cover-container img {
          display: block;
          position: absolute;
          top: 0;
          width: 100%;
          height: 100%;
          object-fit: cover;
        }

        .config-page {
          padding: 1em;
          border: 1px solid #dee2e6;
          border-top: none;
        }

        td {
          padding: 0 0.5em;
        }

        .env-table td {
          padding: 0.25em;
          border-bottom: rgba(0, 0, 0, 0.25) 1px solid;
          vertical-align: top;
        }

        .actions-col {
          min-width: 150px;
        }

        .pre-wrap {
          white-space: pre-wrap;
        }

        .dragover {
          border-top: 2px solid #ffc400;
        }
</style>
