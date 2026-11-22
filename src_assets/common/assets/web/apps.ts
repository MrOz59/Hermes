import { createApp, defineComponent } from 'vue'
import { initApp } from './init'
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
  "use-app-identity": false,
  "per-client-app-identity": false,
  "allow-client-commands": true,
  "virtual-display": false,
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

interface AppList {
  apps: Application[];
  current_app: string;
  host_name: string;
  host_uuid: string;
}

const app = createApp(defineComponent({
  components: {
    Navbar,
    Checkbox
  },
  data() {
    return {
      apps: [] as Application[],
      showEditForm: false,
      actionDisabled: false,
      editForm: null as Application | null,
      detachedCmd: "",
      coverSearching: false,
      coverFinderBusy: false,
      coverCandidates: [] as CoverCandidate[],
      platform: "",
      currentApp: "",
      draggingApp: -1,
      hostName: "",
      hostUUID: "",
      listReordered: false
    };
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
      this.editForm = Object.assign({}, newApp);
      this.showEditForm = true;
    },
    launchApp(event: MouseEvent, app: Application) {
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
}));

app.directive('dropdown-show', {
  mounted: function (el, binding) {
    el.addEventListener('show.bs.dropdown', binding.value);
  }
});

initApp(app);
