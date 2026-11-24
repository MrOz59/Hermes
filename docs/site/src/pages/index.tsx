import Link from '@docusaurus/Link'
import useBaseUrl from '@docusaurus/useBaseUrl'
import Layout from '@theme/Layout'

import styles from './index.module.css'

const guides = [
  {
    title: 'Configure Hermes',
    description: 'Adjust video, audio, input, and virtual displays for your setup.',
    to: '/configuration',
  },
  {
    title: 'Troubleshoot streaming',
    description: 'Diagnose pairing, capture, display, and encoder problems.',
    to: '/troubleshooting',
  },
  {
    title: 'Build from source',
    description: 'Set up the Linux toolchain, build Hermes, and run its tests.',
    to: '/building',
  },
]

export default function Home() {
  const logo = useBaseUrl('/hermes.svg')

  return (
    <Layout description="Install, configure, troubleshoot, and develop Hermes, the self-hosted game-streaming host for Linux.">
      <main className={styles.home}>
        <section className={styles.hero} aria-labelledby="hero-title">
          <div className={styles.heroContent}>
            <h1 id="hero-title">Game streaming<br />on Linux.</h1>
            <p>Set up Hermes on your Linux PC and stream your games to Hestia or another compatible client.</p>
            <div className={styles.actions}>
              <Link className={styles.primaryAction} to="/getting_started">
                Get started <span aria-hidden="true">→</span>
              </Link>
              <Link className={styles.secondaryAction} to="/compatibility">
                Check compatibility
              </Link>
            </div>
          </div>
          <div className={styles.artwork} aria-hidden="true">
            <img className={styles.mark} src={logo} width="224" height="224" alt="" />
          </div>
        </section>

        <section className={styles.guideSection} aria-labelledby="guides-title">
          <div className={styles.sectionHeading}>
            <h2 id="guides-title">Guides and reference</h2>
            <Link to="/api">HTTP API <span aria-hidden="true">→</span></Link>
          </div>
          <ul className={styles.guides}>
            {guides.map(({ title, description, to }) => (
              <li key={to}>
                <Link className={styles.guide} to={to}>
                  <h3>{title}</h3>
                  <p>{description}</p>
                  <span className={styles.guideArrow} aria-hidden="true">→</span>
                </Link>
              </li>
            ))}
          </ul>
        </section>
      </main>
    </Layout>
  )
}
