package akkaserial

import sbt._

object Dependencies {

  val akkaActor = "com.typesafe.akka" %% "akka-actor" % "2.5.21"
  val akkaStream ="com.typesafe.akka" %% "akka-stream" % "2.5.21"

  val akkaTestKit = "com.typesafe.akka" %% "akka-testkit" % "2.5.21"
  val scalatest = "org.scalatest" %% "scalatest" % "3.0.5"

}
